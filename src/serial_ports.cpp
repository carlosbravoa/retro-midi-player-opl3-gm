/*
 * serial_ports.cpp — see serial_ports.hpp.
 */
#include "serial_ports.hpp"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <map>

namespace fs = std::filesystem;

std::string serial_dev_path(const std::string &dev)
{
    return (!dev.empty() && dev[0] == '/') ? dev : "/dev/" + dev;
}

std::string serial_dev_tty(const std::string &dev)
{
    std::error_code ec;
    fs::path p = fs::canonical(serial_dev_path(dev), ec);
    if (ec) p = dev;
    return p.filename().string();
}

std::vector<SerialPort> serial_ports_enumerate()
{
    std::error_code ec;

    // kernel name -> stable by-id path. Missing or unreadable (some sandboxes)
    // just means no stable names.
    std::map<std::string, std::string> by_id;
    for (fs::directory_iterator it("/dev/serial/by-id", ec), end; !ec && it != end; it.increment(ec)) {
        std::error_code ec2;
        fs::path target = fs::canonical(it->path(), ec2);
        if (!ec2) by_id[target.filename().string()] = it->path().string();
    }

    std::vector<SerialPort> out;
    ec.clear();
    for (fs::directory_iterator it("/dev", ec), end; !ec && it != end; it.increment(ec)) {
        std::string n = it->path().filename().string();
        if (n.rfind("ttyACM", 0) != 0 && n.rfind("ttyUSB", 0) != 0) continue;
        // The USB product string sits one (CDC-ACM) or two (usb-serial) levels
        // above the tty's device node.
        std::string desc;
        for (const char *rel : { "/device/../product", "/device/../../product" }) {
            std::ifstream f("/sys/class/tty/" + n + rel);
            if (f && std::getline(f, desc) && !desc.empty()) break;
            desc.clear();
        }
        auto id = by_id.find(n);
        out.push_back(SerialPort{ n, desc, id == by_id.end() ? std::string() : id->second });
    }
    std::sort(out.begin(), out.end(), [](const SerialPort &a, const SerialPort &b) {
        int c = a.name.compare(0, 6, b.name, 0, 6);
        return c ? c < 0 : atoi(a.name.c_str() + 6) < atoi(b.name.c_str() + 6);
    });
    return out;
}

void serial_ports_list()
{
    std::vector<SerialPort> ports = serial_ports_enumerate();
    if (ports.empty()) {
        printf("No USB-serial devices found (ttyACM* / ttyUSB*).\n");
        return;
    }
    printf("USB-serial devices (use with -d):\n");
    for (const SerialPort &p : ports) {
        printf("  %-10s %s\n", p.name.c_str(), p.desc.c_str());
        if (!p.stable.empty()) printf("  %-10s stable name: %s\n", "", p.stable.c_str());
    }
}
