/*
 * serial_ports.hpp — find the USB-serial devices a board could be on.
 *
 * The kernel numbers ttyACM* / ttyUSB* in plug order, so the same board can be
 * ttyUSB0 today and ttyUSB1 tomorrow. udev also publishes a stable name for
 * each under /dev/serial/by-id/ (built from the USB vendor/product/serial);
 * when one exists we prefer it, so a saved port keeps pointing at the same
 * physical device.
 */
#ifndef SERIAL_PORTS_HPP
#define SERIAL_PORTS_HPP

#include <string>
#include <vector>

struct SerialPort {
    std::string name;     // kernel name, "ttyUSB1"
    std::string desc;     // USB product string ("" if unreadable)
    std::string stable;   // "/dev/serial/by-id/..." ("" if there is none)

    // What to open and remember for this port.
    const std::string &device() const { return stable.empty() ? name : stable; }
};

// ttyACM* then ttyUSB*, each in numeric order.
std::vector<SerialPort> serial_ports_enumerate();

// A device as given by the user ("ttyUSB1", "/dev/ttyUSB1", a by-id path, any
// absolute path) -> the path to open.
std::string serial_dev_path(const std::string &dev);

// The kernel name a device currently resolves to ("ttyUSB1"), following
// symlinks; if it doesn't exist right now, just its last path component.
std::string serial_dev_tty(const std::string &dev);

// Print the detected ports (for --list-ports).
void serial_ports_list();

#endif /* SERIAL_PORTS_HPP */
