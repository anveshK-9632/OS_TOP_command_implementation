#include <iostream>
#include <fstream>
#include <string>
#include <sstream>
#include <vector>
#include <filesystem>
#include <iomanip>
#include <utmp.h>
#include <thread>
#include <chrono>
#include <ncurses.h>
#include <dirent.h>
#include <pwd.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>
#include <algorithm>
#include <map>
#include <signal.h>   
#include <cstring>


namespace fs = std::filesystem;


///////////////////////////////////////////////
//Step-1 helper functions(Summary)
//////////////////////////////////////////////

struct SystemSummary {
    std::string upTime;
    std::string loadAvg;
};

struct TaskStatus {
    int total = 0;
    int running = 0;
    int sleeping = 0;
    int stopped = 0;
    int zombie = 0;
};

struct MemoryStats {
    float memTotal, memFree, memUsed, memBuffCache;
    float swapTotal, swapFree, swapUsed, availMem;
};


//Function to get uptime
void getupTime(SystemSummary &stats) {
    std::ifstream uptimeFile("/proc/uptime");
    double total_seconds;
    std::string ans = "uptime: ";
    if (uptimeFile >> total_seconds) {
        int up_secs = (int)total_seconds;
        int days = up_secs / 86400;
        int hours = (up_secs % 86400) / 3600;
        int minutes = (up_secs % 3600) / 60;
        if (days > 0)  {ans += std::to_string(days); ans += " days ";}
        ans += std::to_string(hours); ans += " hours ";
        ans += std::to_string(minutes); ans += " minutes ";
        stats.upTime = ans;
    }

}

//Function to get Load Average
void getLoadAverage(SystemSummary &stats) {
    std::ifstream loadFile("/proc/loadavg");
    //std::getline(loadFile, stats.loadAvg); // Reads the first line containing 3 averages
    std::string one, five, fifteen;
    loadFile >> one >> five >> fifteen;
    stats.loadAvg = one + " " + five + " " + fifteen;
}

//Function to get User Count
int getActiveUserCount() {
    int count = 0;
    struct utmp *up;
    setutent(); // Rewind to the beginning of the utmp file
    while ((up = getutent())) {
        if (up->ut_type == USER_PROCESS) { // Only count actual logged-in users
            count++;
        }
    }
    endutent(); // Close the file
    return count;
}

//Function to get number of tasks
void getDetailedTaskCount(TaskStatus &stats) {
    for (const auto& entry : fs::directory_iterator("/proc")) {
        std::string filename = entry.path().filename().string();
        
        // Check if directory name is a number (PID)
        if (entry.is_directory() && std::isdigit(filename[0])) {
            stats.total++;
            
            // Open /proc/[PID]/stat
            std::ifstream statFile(entry.path().string() + "/stat");
            if (statFile.is_open()) {
                std::string pid, comm, state;
                // Field 1: PID, Field 2: Command (in parens), Field 3: State
                statFile >> pid >> comm >> state;

                if (state == "R") stats.running++;
                else if (state == "S" || state == "D") stats.sleeping++;
                else if (state == "T") stats.stopped++;
                else if (state == "Z") stats.zombie++;
            }
        }
    }
}

//Function to get Memory Information
void getMemoryAndSwap(MemoryStats &m) {
    std::ifstream file("/proc/meminfo");
    std::string key;
    float value;
    std::string unit;

    while (file >> key >> value >> unit) {
        if (key == "MemTotal:") m.memTotal = value / 1024.0;
        else if (key == "MemFree:") m.memFree = value / 1024.0;
        else if (key == "Buffers:") m.memBuffCache += value / 1024.0;
        else if (key == "Cached:") m.memBuffCache += value / 1024.0;
        else if (key == "SReclaimable:") m.memBuffCache += value / 1024.0;
        else if (key == "SwapTotal:") m.swapTotal = value / 1024.0;
        else if (key == "SwapFree:") m.swapFree = value / 1024.0;
        else if (key == "MemAvailable:") m.availMem = value / 1024.0;
    }
    m.memUsed = m.memTotal - m.availMem;
    m.swapUsed = m.swapTotal - m.swapFree;
}




///////////////////////////////////////////////
//Step-2 helper functions(CPU live usage)
//////////////////////////////////////////////
struct CPUData {
    unsigned long long user, nice, system, idle, iowait, irq, softirq, steal;

    unsigned long long getTotalTime() const {
        return user + nice + system + idle + iowait + irq + softirq + steal;
    }
    
    unsigned long long getIdleTime() const {
        return idle + iowait;
    }
};

CPUData readCPUStats() {
    std::ifstream file("/proc/stat");
    std::string cpuLabel;
    CPUData data;
    file >> cpuLabel >> data.user >> data.nice >> data.system >> data.idle 
         >> data.iowait >> data.irq >> data.softirq >> data.steal;
    return data;
}


///////////////////////////////////////////////
//Step-3 helper functions(getting Process information)
//////////////////////////////////////////////

struct Process {
    int pid;
    std::string user;
    int priority;
    int nice;
    long virt; // Virtual Memory in KiB
    long res;  // Resident Memory in KiB
    double cpu_usage = 0.0;
    double mem_usage = 0.0;
    std::string command;
};

// Function to read Individual Process Ticks
long long getProcessTicks(int pid) {
    std::ifstream statFile("/proc/" + std::to_string(pid) + "/stat");
    if (!statFile.is_open()) return 0;
    std::string line;
    std::getline(statFile, line);
    size_t lastParen = line.find_last_of(')');
    if (lastParen == std::string::npos) return 0;
    std::stringstream ss(line.substr(lastParen + 2));
    std::string dummy;
    long long utime, stime;
    for(int i=0; i<11; i++) ss >> dummy; 
    ss >> utime >> stime;
    return utime + stime;
}


std::string getUsername(uid_t uid) {
    struct passwd *pw = getpwuid(uid);
    return pw ? pw->pw_name : std::to_string(uid);
}

std::vector<Process> getProcessList(float totalMemMiB) {
    std::vector<Process> procs;
    for (const auto& entry : fs::directory_iterator("/proc")) {
        std::string pidStr = entry.path().filename().string();
        if (std::isdigit(pidStr[0])) {
            Process p;
            p.pid = std::stoi(pidStr);

            // 1. Get User
            struct stat info;
            if (::stat(entry.path().c_str(), &info) == 0) {
                p.user = getUsername(info.st_uid);
            }

            // 2. Get PR and NI from /proc/[PID]/stat
            std::ifstream statFile("/proc/" + pidStr + "/stat");
            std::string line;
            std::getline(statFile, line);
            size_t lastParen = line.find_last_of(')');
            std::stringstream ss(line.substr(lastParen + 2));
            
            std::string dummy;
            // Fields are relative to the end of the command name
            for(int i=0; i<15; i++) ss >> dummy; 
            ss >> p.priority >> p.nice;

            // 3. Get Memory from /proc/[PID]/statm
            std::ifstream statmFile("/proc/" + pidStr + "/statm");
            long size, resident;
            statmFile >> size >> resident; 
            // Pages to KiB (typically 4KB per page)
            long pageSizeKiB = sysconf(_SC_PAGESIZE) / 1024;
            p.virt = size * pageSizeKiB;
            p.res = resident * pageSizeKiB;
            p.mem_usage = (p.res / 1024.0 / totalMemMiB) * 100.0;

            // 4. Command
            std::ifstream cmdFile("/proc/" + pidStr + "/comm");
            std::getline(cmdFile, p.command);

            procs.push_back(p);
        }
    }
    return procs;
}


int main() {

    //Initialize ncurses
    initscr();             // Start ncurses mode
    noecho();              // Don't echo keypresses to the screen
    curs_set(0);           // Hide the blinking cursor
    nodelay(stdscr, TRUE); // Make getch() "non-blocking" (don't wait for input)
    keypad(stdscr, TRUE);  // Enable arrow keys/special keys


    //Get Total System Memory (needed for %MEM calculation)
    float totalSystemMem = 0;
    std::ifstream memInfo("/proc/meminfo");
    std::string key;
    float value;
    while (memInfo >> key >> value) {
        if (key == "MemTotal:") {
            totalSystemMem = value / 1024.0; // Convert KB to MiB
            break; 
        }
    }

    bool running = true;
    int refreshMillis =1000;
    CPUData s1 = readCPUStats(); // Initial snapshot
    std::map<int, long long> prevProcTicks;
    CPUData prevSystem = readCPUStats();

    int maxdisplay = 15;

    bool sortByMemory = false;

    while (running) {
        // A. Check for user input
        int ch = getch();

        // === Implementation of 'q' Command: Quit the Application ===

        if (ch == 'q' || ch == 'Q') {
            running = false;
        }

        // === Implementation of 'd' Command: Change Refresh Interval ===

        else if (ch == 'd' || ch == 'D') {
        // 1. Temporary disable non-blocking to get user input
        nodelay(stdscr, FALSE);
        echo(); // Show what the user types
        curs_set(1); // Show cursor

        mvprintw(0, 0, "Change delay from %.1fs to: ", refreshMillis / 1000.0);
        clrtoeol(); // Clear the rest of the line

        char input[10];
        getnstr(input, 9); // Get string input safely

        try {
            float newDelay = std::stof(input);
            if (newDelay > 0) {
                refreshMillis = static_cast<int>(newDelay * 1000);
            }
        } catch (...) {
            // If user enters non-numeric, just keep the old delay
        }

        // 2. Restore TUI settings
        noecho();
        nodelay(stdscr, TRUE);
        curs_set(0);
        }

        // === Implementation of 'k' Command: Kill Process by PID ===
        
        else if (ch == 'k' || ch == 'K'){
            // 1. Switch to input mode
            nodelay(stdscr, FALSE);
            echo();
            curs_set(1);

            // 2. Ask for PID
            mvprintw(0, 0, "Enter PID to kill: ");
            clrtoeol();
            refresh();   

            char input[10];
            getnstr(input, 9);

            int pid = 0;
            bool success = false;

            try {
                pid = std::stoi(input);

                // 3. Try killing process
                if (kill(pid, SIGTERM) == 0) {
                    success = true;
                }
            } catch (...) {
                // invalid input handled below
            }

            // 4. Show result message (safe row)
            mvprintw(2, 0, "                                          "); 
            if (success) {
                mvprintw(maxdisplay+19, 0, "Process %d killed successfully!", pid);
            } else {
                mvprintw(maxdisplay+19, 0, "Failed to kill process or invalid PID!");
            }

            refresh();  

            // 5. Pause so user can SEE the message
            std::this_thread::sleep_for(std::chrono::milliseconds(1500));

            // 6. Restore ncurses mode
            noecho();
            nodelay(stdscr, TRUE);
            curs_set(0);
        }

        // === Implementation of 'n' Command: Change Number of Displayed Processes ===
        else if (ch == 'n' || ch == 'N')
        {
            // 1. Enable input mode
            nodelay(stdscr, FALSE);
            echo();
            curs_set(1);

            // 2. Ask user
            mvprintw(0, 0, "Change number of processes to: ");
            clrtoeol();
            refresh();

            char input[10];
            getnstr(input, 9);

            try {
                int newLimit = std::stoi(input);

                if (newLimit > 0) {
                    maxdisplay = newLimit;

                    mvprintw(maxdisplay+18, 0, "Now showing %d processes.", maxdisplay);
                } else {
                    mvprintw(maxdisplay+18, 0, "Invalid number!");
                }
            } catch (...) {
                mvprintw(maxdisplay+18, 0, "Invalid input!");
            }

            refresh();
            std::this_thread::sleep_for(std::chrono::milliseconds(1200));

            // 3. Restore mode
            noecho();
            nodelay(stdscr, TRUE);
            curs_set(0);
        }

        // === Implementation of 'M' Command: Toggle Sort by Memory Usage ===
        else if (ch == 'm' || ch == 'M')
        {
            sortByMemory = !sortByMemory;  // toggle

            // Show current mode
            mvprintw(2, 0, "                                             ");  // clear line
            if (sortByMemory) {
                mvprintw(maxdisplay+18, 0, "Sorting by MEMORY usage.");
            } else {
                mvprintw(maxdisplay+18, 0, "Sorting by CPU usage.");
            }

            refresh();
            std::this_thread::sleep_for(std::chrono::milliseconds(1200));
        }


        // B. Update Data

        //Get Step-1 Data
        SystemSummary stats;
        getupTime(stats);// 1. Get Uptime
        int userCount = getActiveUserCount();// 2. Get Usercount
        getLoadAverage(stats);// 3. Get Load Average
        TaskStatus taskStats;
        getDetailedTaskCount(taskStats);// 4. Get TaskCount
        MemoryStats m = {0};
        getMemoryAndSwap(m);// 5. Memory and Swap

        //Get Step-2 Data
        CPUData s2 = readCPUStats();
        unsigned long long totalDelta = s2.getTotalTime() - s1.getTotalTime();
        unsigned long long idleDelta = s2.getIdleTime() - s1.getIdleTime();
        double usage = 100.0 * (totalDelta - idleDelta) / totalDelta;
        double userPerc = 100.0 * (s2.user - s1.user) / totalDelta;
        double sysPerc = 100.0 * (s2.system - s1.system) / totalDelta;
        double idlePerc = 100.0 * (s2.idle - s1.idle) / totalDelta;

        //Get Step-3 Data
        CPUData currSystem = readCPUStats();
        unsigned long long systemDelta = currSystem.getTotalTime() - prevSystem.getTotalTime();
        std::vector<Process> currentProcs = getProcessList(totalSystemMem);
        for (auto& p : currentProcs) {
            long long currTicks = getProcessTicks(p.pid);
            if (prevProcTicks.count(p.pid) && systemDelta > 0) {
                p.cpu_usage = (100.0 * (currTicks - prevProcTicks[p.pid])) / systemDelta;
            }
            prevProcTicks[p.pid] = currTicks;
        }

        // ========= Sorting Logic: Memory vs CPU =========
        if (sortByMemory){
            std::sort(currentProcs.begin(), currentProcs.end(),[](const Process& a, const Process& b){
                    return a.mem_usage > b.mem_usage;
            });
        }
        else{
            std::sort(currentProcs.begin(), currentProcs.end(),[](const Process& a, const Process& b){
                    return a.cpu_usage > b.cpu_usage;
            });
        }

        // C. Render to Screen
        clear(); // Clear the previous frame

        //Step-1 output printing
        mvprintw(0, 0, "%s\n", stats.upTime.c_str());
        mvprintw(1, 0, "Number of users: %d\n", userCount);
        mvprintw(2, 0, "load average: %s\n", stats.loadAvg.c_str());
        mvprintw(3, 0, "Tasks: %d total, %3d running, %3d sleeping, %3d stopped, %3d zombie\n",
        taskStats.total, taskStats.running, taskStats.sleeping, 
        taskStats.stopped, taskStats.zombie);
        mvprintw(4, 0, "MiB Mem : %8.1f total, %8.1f free, %8.1f used, %8.1f buff/cache\n",
            m.memTotal, m.memFree, m.memUsed, m.memBuffCache);
        mvprintw(5, 0, "MiB Swap: %8.1f total, %8.1f free, %8.1f used. %8.1f avail Mem\n",
            m.swapTotal, m.swapFree, m.swapUsed, m.availMem);

        //Step-2 Output printing
        mvprintw(6,0, "%%Cpu(s): %5.1f us, %5.1f sy, %5.1f ni, %5.1f id\n", 
            userPerc, sysPerc, 100.0 * (s2.nice - s1.nice) / totalDelta, idlePerc);
        
        //Step-3 Output printing
        mvprintw(8, 0, "List of Processes and their Info");
        mvprintw(10, 0, "%-7s %-10s %-4s %-4s %-8s %-8s %-6s %-6s %-15s", 
         "PID", "USER", "PR", "NI", "VIRT", "RES", "%CPU", "%MEM", "COMMAND");
        
        for (int i = 0; i < maxdisplay && i < currentProcs.size(); ++i) {
            const auto& p = currentProcs[i];
            mvprintw(11 + i, 0, "%-7d %-10s %-4d %-4d %-8ld %-8ld %-6.1f %-6.1f %-15s", 
            p.pid, 
            p.user.substr(0, 9).c_str(), 
            p.priority, 
            p.nice, 
            p.virt, 
            p.res, 
            p.cpu_usage, 
            p.mem_usage, 
            p.command.substr(0, 14).c_str());
        }
        mvprintw(maxdisplay+20, 0, "Press 'q' to exit.");
        
        refresh(); // Push all changes to the actual terminal

        // D. Prepare for next iteration
        s1 = s2; // Current snapshot becomes the 'old' one for the next second
        prevSystem = currSystem;
        std::this_thread::sleep_for(std::chrono::milliseconds(refreshMillis));
    }

    //Cleanup ncurses before exiting
    endwin();
    return 0;
}