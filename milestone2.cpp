#include <iostream>
#include <iomanip>
#include <string>
#include <fstream>
#include <vector>
#include <sstream>
#include <cmath>
#include <cstdint>

using namespace std;

// ====================== Milestone 2: helpers & structs ======================

struct PTE {
    int  physPage;
    bool valid;
    bool everUsed;   // to count unique used entries
};

struct PhysPageInfo {
    bool free;
    int  ownerProc;   // which process (trace index)
    int  ownerVPage;  // which virtual page
};

struct ProcessInfo {
    ifstream file;
    bool done;
    string filename;
};

// Trim helper (for safety)
static inline string trim(const string &s) {
    size_t b = s.find_first_not_of(" \t\r\n");
    if (b == string::npos) return "";
    size_t e = s.find_last_not_of(" \t\r\n");
    return s.substr(b, e - b + 1);
}

// Parse EIP line: returns true if a valid address was parsed.
bool parseEipLine(const string &line, uint32_t &addrOut) {
    // We try to find the first 8-hex-digit token after ':'.
    size_t colon = line.find(':');
    if (colon == string::npos) return false;

    string after = trim(line.substr(colon + 1));
    if (after.empty()) return false;

    stringstream ss(after);
    string token;
    // The first token after ':' should be the address
    if (!(ss >> token)) return false;

    // Some traces might have longer hex, but we only need lower 32 bits
    // so we accept tokens of length >= 8 and take last 8 chars.
    if (token.size() < 8) return false;

    string addrHex = token.substr(token.size() - 8); // last 8 chars
    // must be hex
    if (addrHex.find_first_not_of("0123456789abcdefABCDEF") != string::npos)
        return false;

    stringstream hx;
    hx << std::hex << addrHex;
    hx >> addrOut;
    return true;
}

// Parse memory line: dstM and srcM
// Sets hasDst/hasSrc and their addresses (if valid).
void parseMemLine(const string &line,
                  bool &hasDst, uint32_t &dstAddr,
                  bool &hasSrc, uint32_t &srcAddr) {
    // Handles lines like:
    // dstM: 7ffdf034 00000000    srcM: 7ffdfe2c 901e8b00
    // or sometimes only dstM part is present.
    hasDst = false;
    hasSrc = false;

    string s = line;
    size_t posDst = s.find("dstM:");
    size_t posSrc = s.find("srcM:");

    auto parseOne = [](const string &sub, uint32_t &addrOut, bool &hasAddr) {
        stringstream ss(sub);
        string label, addrHex, data;
        if (!(ss >> label >> addrHex >> data)) {
            hasAddr = false;
            return;
        }
        // invalid if data is dashed or addr is not hex/zero
        if (data == "--------") {
            hasAddr = false;
            return;
        }
        if (addrHex.size() < 8) {
            hasAddr = false;
            return;
        }
        string addr8 = addrHex.substr(addrHex.size() - 8);
        if (addr8.find_first_not_of("0123456789abcdefABCDEF") != string::npos) {
            hasAddr = false;
            return;
        }
        stringstream hx;
        hx << std::hex << addr8;
        hx >> addrOut;
        if (addrOut == 0) { // ignore zero addresses as per email
            hasAddr = false;
            return;
        }
        hasAddr = true;
    };

    if (posDst != string::npos) {
        size_t end = (posSrc != string::npos) ? posSrc : string::npos;
        string sub = trim(s.substr(posDst, end - posDst));
        parseOne(sub, dstAddr, hasDst);
    }
    if (posSrc != string::npos) {
        string sub = trim(s.substr(posSrc));
        parseOne(sub, srcAddr, hasSrc);
    }
}

// Find an index of a free physical page, or -1 if none.
int findFreePhysPage(vector<PhysPageInfo> &physPages, long long systemPages) {
    for (size_t i = (size_t)systemPages; i < physPages.size(); ++i) {
        if (physPages[i].free)
            return (int)i;
    }
    return -1;
}

// Choose a victim physical page using simple round-robin over user pages
int chooseVictimPage(vector<PhysPageInfo> &physPages,
                     long long systemPages,
                     int &nextVictim) {
    int n = (int)physPages.size();
    if (n <= (int)systemPages) return -1;

    for (int attempts = 0; attempts < n; ++attempts) {
        if (nextVictim < (int)systemPages || nextVictim >= n)
            nextVictim = (int)systemPages;

        int idx = nextVictim;
        nextVictim++;
        if (nextVictim >= n) nextVictim = (int)systemPages;

        // Only evict pages used by user processes (non-free and not system)
        if (!physPages[idx].free && idx >= systemPages) {
            return idx;
        }
    }
    return -1;
}

// Simulate one memory access for process
void simulateAccess(int proc,
                    uint32_t addr,
                    long long &virtualPagesMapped,
                    long long &pageTableHits,
                    long long &pagesFromFree,
                    long long &totalPageFaults,
                    vector<vector<PTE>> &pageTables,
                    vector<PhysPageInfo> &physPages,
                    vector<long long> &usedEntryCount,
                    long long systemPages,
                    int &nextVictimIdx) {
    const int PAGE_SIZE = 4096;

    if (addr == 0) return; // ignore zero addresses

    int vpage = (int)(addr / PAGE_SIZE);
    if (vpage < 0 || vpage >= (int)pageTables[proc].size()) return;

    virtualPagesMapped++;

    PTE &entry = pageTables[proc][vpage];
    if (entry.valid) {
        pageTableHits++;
        return;
    }

    // Miss: need a physical page
    int freeIdx = findFreePhysPage(physPages, systemPages);
    if (freeIdx != -1) {
        // From free list
        physPages[freeIdx].free       = false;
        physPages[freeIdx].ownerProc  = proc;
        physPages[freeIdx].ownerVPage = vpage;

        entry.valid    = true;
        entry.physPage = freeIdx;

        if (!entry.everUsed) {
            entry.everUsed = true;
            usedEntryCount[proc]++;
        }

        pagesFromFree++;
    } else {
        // Page fault: must evict a victim
        int victimIdx = chooseVictimPage(physPages, systemPages, nextVictimIdx);
        if (victimIdx == -1) {
            return; // no victim; should not happen if user pages exist
        }

        int vProc  = physPages[victimIdx].ownerProc;
        int vVpage = physPages[victimIdx].ownerVPage;

        // Unmap victim
        if (vProc >= 0 && vVpage >= 0) {
            PTE &victimEntry = pageTables[vProc][vVpage];
            victimEntry.valid    = false;
            victimEntry.physPage = -1;
        }

        // Map victim page to current process
        physPages[victimIdx].ownerProc  = proc;
        physPages[victimIdx].ownerVPage = vpage;

        entry.valid    = true;
        entry.physPage = victimIdx;

        if (!entry.everUsed) {
            entry.everUsed = true;
            usedEntryCount[proc]++;
        }

        totalPageFaults++;
    }
}

// Run the full virtual memory simulation for Milestone 2
void runVirtualMemorySim(long long physMemBytes,
                         long long numPhysPages,
                         long long systemPages,
                         int pteBits,
                         int timeSlice,
                         const vector<string> &traceFiles) {
    const int PAGE_SIZE   = 4096;
    const int NUM_VPAGES  = 512 * 1024;  // 512K entries per process

    int numTraces = (int)traceFiles.size();
    if (numTraces == 0) {
        cout << "\n***** VIRTUAL MEMORY SIMULATION RESULTS *****\n\n";
        cout << "No trace files provided.\n";
        return;
    }

    long long userPages = numPhysPages - systemPages;

    // Physical page info
    vector<PhysPageInfo> physPages((size_t)numPhysPages);
    for (long long i = 0; i < numPhysPages; ++i) {
        if (i < systemPages) {
            physPages[(size_t)i].free       = false; // OS-owned
            physPages[(size_t)i].ownerProc  = -1;
            physPages[(size_t)i].ownerVPage = -1;
        } else {
            physPages[(size_t)i].free       = true;  // free for user
            physPages[(size_t)i].ownerProc  = -1;
            physPages[(size_t)i].ownerVPage = -1;
        }
    }

    // Per-process page tables
    vector<vector<PTE>> pageTables(numTraces,
                                   vector<PTE>(NUM_VPAGES, { -1, false, false }));
    vector<long long> usedEntryCount(numTraces, 0);

    // Stats
    long long virtualPagesMapped = 0;
    long long pageTableHits      = 0;
    long long pagesFromFree      = 0;
    long long totalPageFaults    = 0;

    // Open trace files
    vector<ProcessInfo> procs(numTraces);
    int active = 0;
    for (int i = 0; i < numTraces; ++i) {
        procs[i].filename = traceFiles[i];
        procs[i].file.open(traceFiles[i].c_str());
        if (procs[i].file.is_open()) {
            procs[i].done = false;
            active++;
        } else {
            procs[i].done = true;
            cerr << "ERROR: could not open trace file: " << traceFiles[i] << "\n";
        }
    }

    if (active == 0) {
        cout << "\n***** VIRTUAL MEMORY SIMULATION RESULTS *****\n\n";
        cout << "ERROR: No trace files could be opened.\n";
        return;
    }

    // Time-slice / round-robin simulation
    int current       = 0;
    int nextVictimIdx = (int)systemPages; // start evictions after system pages

    while (active > 0 && numPhysPages > systemPages) {
        if (procs[current].done) {
            current = (current + 1) % numTraces;
            continue;
        }

        int instrCount = 0;
        while ((timeSlice == -1 || instrCount < timeSlice) && !procs[current].done) {
            string eipLine, memLine;
            if (!getline(procs[current].file, eipLine)) {
                procs[current].done = true;
                active--;
                break;
            }
            if (!getline(procs[current].file, memLine)) {
                procs[current].done = true;
                active--;
                break;
            }

            eipLine = trim(eipLine);
            memLine = trim(memLine);

            // EIP access
            uint32_t eipAddr;
            if (parseEipLine(eipLine, eipAddr)) {
                simulateAccess(current, eipAddr,
                               virtualPagesMapped, pageTableHits,
                               pagesFromFree, totalPageFaults,
                               pageTables, physPages, usedEntryCount,
                               systemPages, nextVictimIdx);
            }

            // dstM / srcM access
            bool hasDst, hasSrc;
            uint32_t dstAddr = 0, srcAddr = 0;
            parseMemLine(memLine, hasDst, dstAddr, hasSrc, srcAddr);

            if (hasDst) {
                simulateAccess(current, dstAddr,
                               virtualPagesMapped, pageTableHits,
                               pagesFromFree, totalPageFaults,
                               pageTables, physPages, usedEntryCount,
                               systemPages, nextVictimIdx);
            }
            if (hasSrc) {
                simulateAccess(current, srcAddr,
                               virtualPagesMapped, pageTableHits,
                               pagesFromFree, totalPageFaults,
                               pageTables, physPages, usedEntryCount,
                               systemPages, nextVictimIdx);
            }

            instrCount++;
        }

        current = (current + 1) % numTraces;
    }

    // Close trace files
    for (int i = 0; i < numTraces; ++i) {
        if (procs[i].file.is_open())
            procs[i].file.close();
    }

    // Print results
    cout << "\n***** VIRTUAL MEMORY SIMULATION RESULTS *****\n" << endl;
    cout << "Physical Pages Used By SYSTEM:\t" << systemPages << endl;
    cout << "Pages Available to User:\t"    << userPages   << endl;

    cout << "\nVirtual Pages Mapped:\t\t"    << virtualPagesMapped << endl;
    cout << "\t------------------------------" << endl;
    cout << "\tPage Table Hits:\t"    << pageTableHits   << endl;
    cout << "\tPages from Free:\t"    << pagesFromFree   << endl;
    cout << "\tTotal Page Faults:\t"  << totalPageFaults << endl;

    cout << "\nPage Table Usage Per Process:\n";
    cout << "------------------------------\n";

    long long totalPteBytesPerProc = (long long)NUM_VPAGES * pteBits / 8;

    cout << fixed << setprecision(2);
    for (int i = 0; i < numTraces; ++i) {
        cout << "[" << i << "] " << traceFiles[i] << ":\n";

        double percentUsed = (usedEntryCount[i] * 100.0) / NUM_VPAGES;
        long long usedBytes   = (long long)usedEntryCount[i] * pteBits / 8;
        long long wastedBytes = totalPteBytesPerProc - usedBytes;

        cout << "\tUsed Page Table Entries:\t" << usedEntryCount[i]
             << "  (" << percentUsed << "%)\n";
        cout << "\tPage Table Wasted:\t" << wastedBytes << " bytes\n\n";
    }
}

// ====================== Milestone 2 main with Milestone1 ======================

int main(int argc, char* argv[]) {
    if (argc < 2) {
        cerr << "Usage: " << argv[0]
             << " trace1.trc [trace2.trc] [trace3.trc]\n";
        return 1;
    }

    vector<string> traceFiles;
    for (int i = 1; i < argc; ++i) {
        traceFiles.push_back(argv[i]);
    }

    // ===== Milestone 1 parameters =====
    int cacheSize = 512;
    int blockSize = 16;          
    int associativity = 4;
    string replacementPolicy = "Round Robin";
    int physMemory = 1024;       // MB
    double percentUsed = 75.0;   // %
    int timeSlice = 100;         // instructions per time slice

    const int PAGE_SIZE = 4096;

    long long cacheBytes = (long long)cacheSize * 1024;
    long long physMemBytes = (long long)physMemory * 1024 * 1024;

    long long totalBlocks = cacheBytes / blockSize;
    long long rows = totalBlocks / associativity;

    int offsetBits = (int)log2(blockSize);
    int indexBits = (int)log2(rows);
    int physAddrBits = (int)log2(physMemBytes);
    int tagBits = physAddrBits - indexBits - offsetBits;

    long long overheadBytes = (long long)(rows * associativity * (tagBits + 1)) / 8;
    long long implMemBytes = cacheBytes + overheadBytes;
    double implMemKB = implMemBytes / 1024.0;
    double cost = implMemKB * 0.07;

    long long numPhysPages = physMemBytes / PAGE_SIZE;
    long long systemPages = (long long)(numPhysPages * (percentUsed / 100.0));
    int pteBits = 1 + (int)log2(numPhysPages);
    long long totalPageTableRAM = (long long)((512.0 * 1024 * 3 * pteBits) / 8.0);

    // ===== Milestone 1 =====
    cout << "Cache Simulator - CS 3853 - Team #03" << endl;
    cout << "\nTrace File(s):" << endl;
    for (size_t i = 0; i < traceFiles.size(); ++i) {
        cout << "\t" << traceFiles[i] << endl;
    }

    cout << "\n***** Cache Input Parameters *****\n" << endl;
    cout << "Cache Size:\t\t\t" << cacheSize << " KB" << endl;
    cout << "Block Size:\t\t\t" << blockSize << " bytes" << endl;
    cout << "Associativity:\t\t\t" << associativity << endl;
    cout << "Replacement Policy:\t\t" << replacementPolicy << endl;
    cout << "Physical Memory:\t\t" << physMemory << " MB" << endl;
    cout << fixed << setprecision(1);
    cout << "Percent Memory Used by System:\t" << percentUsed << "%" << endl;
    cout << "Instructions / Time Slice:\t" << timeSlice << endl;

    cout << "\n***** Cache Calculated Values *****" << endl;
    cout << "Total # Blocks:\t\t\t" << totalBlocks << endl;
    cout << "Tag Size:\t\t\t" << tagBits << " bits" << endl;
    cout << "Index Size:\t\t\t" << indexBits << " bits" << endl;
    cout << "Total # Rows:\t\t\t" << rows << endl;
    cout << "Overhead Size:\t\t\t" << overheadBytes << " bytes" << endl;
    cout << fixed << setprecision(2);
    cout << "Implementation Memory Size:\t" << implMemKB
         << " KB (" << implMemBytes << " bytes)" << endl;
    cout << "Cost:\t\t\t\t$" << cost << " @ $0.07 per KB" << endl;

    cout << "\n***** Physical Memory Calculated Values *****\n" << endl;
    cout << "Number of Physical Pages:\t" << numPhysPages << endl;
    cout << "Number of Pages for System:\t" << systemPages << endl;
    cout << "Size of Page Table Entry:\t" << pteBits << " bits" << endl;
    cout << "Total RAM for Page Table(s):\t" << totalPageTableRAM << " bytes" << endl;

    // ===== Milestone 2: Virtual Memory simulation =====
    runVirtualMemorySim(physMemBytes, numPhysPages, systemPages,
                        pteBits, timeSlice, traceFiles);

    return 0;
}
