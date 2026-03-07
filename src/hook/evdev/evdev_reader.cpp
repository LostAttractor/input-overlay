/*************************************************************************
 * This file is part of input-overlay
 * git.vrsal.cc/alex/input-overlay
 * based on https://codeberg.org/MaxCross/obs-input-overlay-zig
 * Copyright 2026 univrsal <uni@vrsal.cc>.
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, version 2 of the License.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *************************************************************************/

#include "evdev_reader.hpp"

#include "evdev_keycode_map.hpp"
#include <fcntl.h>
#include <linux/input.h>
#include <unistd.h>
#include "util/log.h"

EvdevReader::EvdevReader() = default;

EvdevReader::~EvdevReader()
{
    stop();
}

void EvdevReader::start(const std::string &device_path)
{
    stop();
    running_ = true;
    thread_ = std::thread(&EvdevReader::reader_thread, this, device_path);
}

void EvdevReader::stop()
{
    running_ = false;

    // Close the file descriptor so that a blocking read() in the reader
    // thread is interrupted with an error and the thread can exit.
    int fd_copy = fd_.exchange(-1);
    if (fd_copy >= 0) {
        close(fd_copy);
    }

    if (thread_.joinable()) {
        thread_.join();
    }
}

bool EvdevReader::is_running() const
{
    return running_;
}

bool EvdevReader::is_key_pressed(uint16_t evdev_code) const
{
    if (evdev_code > EVDEV_KEY_MAX)
        return false;
    std::lock_guard<std::mutex> lock(mutex_);
    auto uihook_code = evdev_to_uiohook(evdev_code);
    if (key_state.find(uihook_code) != key_state.end()) {
        return key_state.at(uihook_code);
    }
    return false;
}

int32_t EvdevReader::get_rel_state(uint16_t rel_code) const
{
    if (rel_code > EVDEV_REL_MAX)
        return 0;
    std::lock_guard<std::mutex> lock(mutex_);
    return rel_state_[rel_code];
}

void EvdevReader::set_rel_state(uint16_t rel_code, int32_t value)
{
    if (rel_code > EVDEV_REL_MAX)
        return;
    std::lock_guard<std::mutex> lock(mutex_);
    rel_state_[rel_code] = value;
}

void EvdevReader::reader_thread(const std::string &path)
{
    int fd = open(path.c_str(), O_RDONLY);
    if (fd < 0) {
        running_ = false;
        return;
    }
    fd_.store(fd);

    struct input_event ev {};

    while (running_) {
        ssize_t n = read(fd, &ev, sizeof(ev));
        if (n != static_cast<ssize_t>(sizeof(ev))) {
            // Read error or fd closed by stop() — exit.
            break;
        }

        std::lock_guard lock(mutex_);
        auto uihook_code = evdev_to_uiohook(ev.code);
        binfo("Read evdev event: type %u, code %u, value %d, uihook: 0x%X", ev.type, ev.code, ev.value, uihook_code);
        switch (ev.type) {
        case EV_KEY:
            if (ev.code <= EVDEV_KEY_MAX) {
                if (ev.value == 1) { // pressed
                    key_state[uihook_code] = true;
                    binfo("Key pressed: evdev code %u, uiohook code %u", ev.code, uihook_code);
                } else if (ev.value == 0) { // released
                    key_state[uihook_code] = false;
                    binfo("Key released: evdev code %u, uiohook code %u", ev.code, uihook_code);
                }
                // value == 2 is repeat — we keep the key as pressed.
            }
            break;

        case EV_REL:
            if (ev.code <= EVDEV_REL_MAX) {
                rel_state_[ev.code] = ev.value;
            }
            break;

        default:
            break;
        }
    }

    // Only close if stop() hasn't already closed it
    int expected = fd;
    if (fd_.compare_exchange_strong(expected, -1)) {
        close(fd);
    }
    running_ = false;
}
