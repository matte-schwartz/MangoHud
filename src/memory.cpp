#include <array>
#include <cstdio>
#include <fstream>
#include <map>
#include <spdlog/spdlog.h>
#include <string>
#include <unistd.h>
#include <vector>

#include "memory.h"
#include "file_utils.h"
#include "hud_elements.h"
#include "string_utils.h"

float memused, memmax, swapused;
int mem_temp;
uint64_t proc_mem_resident, proc_mem_shared, proc_mem_virt;
uint64_t game_mem_used;

void update_meminfo() {
    std::ifstream file("/proc/meminfo");
    std::map<std::string, float> meminfo;

    if (!file.is_open()) {
        SPDLOG_ERROR("can't open /proc/meminfo");
        return;
    }

    for (std::string line; std::getline(file, line);) {
        auto key = line.substr(0, line.find(":"));
        auto val = line.substr(key.length() + 2);
        meminfo[key] = std::stoull(val) / 1024.f / 1024.f;
    }

    memmax = meminfo["MemTotal"];
    memused = meminfo["MemTotal"] - meminfo["MemAvailable"];
    swapused = meminfo["SwapTotal"] - meminfo["SwapFree"];
}

void update_mem_temp() {
    static bool inited = false;
    static std::vector<std::ifstream> mem_temp_files;

    if (!inited) {
        inited = true;
        std::string path = "/sys/class/hwmon/";
        auto dirs = ls(path.c_str(), "hwmon", LS_DIRS);
        for (auto &dir : dirs) {
            if (read_line(path + dir + "/name") == "spd5118")
                mem_temp_files.emplace_back(path + dir + "/temp1_input");
        }
        if (mem_temp_files.empty())
            SPDLOG_ERROR("failed to find known ram temp sensors");
    }

    int temp = 0;
    for (auto &file : mem_temp_files) {
        int _temp;
        file.clear();
        file.seekg(0);
        if ((file >> _temp) && _temp > temp)
            temp = _temp;
    }
    mem_temp = temp / 1000;
}

void update_procmem()
{
    auto page_size = sysconf(_SC_PAGESIZE);
    if (page_size < 0) page_size = 4096;

    std::string f = "/proc/";

    {
        auto gs_pid = HUDElements.g_gamescopePid;
        f += gs_pid < 1 ? "self" : std::to_string(gs_pid);
        f += "/statm";
    }

    std::ifstream file(f);

    if (!file.is_open()) {
        SPDLOG_ERROR("can't open {}", f);
        return;
    }

    size_t last_idx = 0;
    std::string line;
    std::getline(file, line);

    if (line.empty())
        return;

    std::array<uint64_t, 3> meminfo;

    for (auto i = 0; i < 3; i++) {
        auto idx = line.find(" ", last_idx);
        auto val = line.substr(last_idx, idx);

        meminfo[i] = std::stoull(val) * page_size;
        last_idx = idx + 1;
    }

    proc_mem_virt = meminfo[0];
    proc_mem_resident = meminfo[1];
    proc_mem_shared = meminfo[2];
}

// Steam can launch games in their own app-steam-app<appid>-<reaperpid>.scope
static std::string steam_app_cgroup(const std::string& proc)
{
    std::ifstream file(proc + "/cgroup");

    for (std::string line; std::getline(file, line);) {
        if (!starts_with(line, "0::"))
            continue;

        auto path = line.substr(3);
        auto scope = path.substr(path.rfind('/') + 1);
        uint32_t appid = 0;
        int end = 0;
        if (sscanf(scope.c_str(), "app-steam-app%u-%*d.scope%n", &appid, &end) == 1 &&
            appid && end == (int)scope.size())
            return "/sys/fs/cgroup" + path;
    }

    return {};
}

// anon + shmem: what reclaim can't drop, plus GPU buffers on shmem-backed drivers
static bool read_game_mem(const std::string& cgroup, uint64_t& used)
{
    std::ifstream file(cgroup + "/memory.stat");
    if (!file.is_open())
        return false;

    uint64_t anon = 0, shmem = 0;
    std::string key;
    uint64_t val;
    while (file >> key >> val) {
        if (key == "anon")
            anon = val;
        else if (key == "shmem")
            shmem = val;
    }

    used = anon + shmem;
    return true;
}

void update_game_mem()
{
    static std::string cached_proc, cgroup;

    auto gs_pid = HUDElements.g_gamescopePid;
    std::string proc = "/proc/" + (gs_pid < 1 ? std::string("self") : std::to_string(gs_pid));

    if (proc != cached_proc || cgroup.empty()) {
        cached_proc = proc;
        cgroup = steam_app_cgroup(proc);
        if (!cgroup.empty())
            SPDLOG_DEBUG("game memory from {}", cgroup);
    }

    uint64_t used = 0;
    if (!cgroup.empty() && !read_game_mem(cgroup, used))
        cgroup.clear();

    game_mem_used = used;
}
