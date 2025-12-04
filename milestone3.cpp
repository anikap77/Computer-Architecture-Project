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
    bool everUsed;   // for counting unique used entries
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

// Trim helper
static inline string trim(const string &s) {
    size_t b = s.find_first_not_of(" \t\r\n");
    if (b == string::npos) return "";
    size_t e = s.find_last_not_of(" \t\r\n");
    return s.substr(b, e - b + 1);
}

// Parse EIP line: returns true if a valid address was parsed.
bool parseEipLine(const string &line, uint32_t &addrOut) {
    size_t colon = line.find(':');
    if (colon == string::npos) return false;

    string after = trim(line.substr(colon + 1));
    if (after.empty()) return false;

    stringstream ss(after);
    string token;
    if (!(ss >> token)) return false;

    if (token.size() < 8) return false;

    string addrHex = token.substr(token.size() - 8); // last 8 chars
    if (addrHex.find_first_not_of("0123456789abcdefABCDEF") != string::npos)
        return false;

    stringstream hx;
    hx << std::hex << addrHex;
    hx >> addrOut;
    return true;
}

// Parse dstM / srcM line
void parseMemLine(const string &line,
                  bool &hasDst, uint32_t &dstAddr,
                  bool &hasSrc, uint32_t &srcAddr) {
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

        // If data is "--------", treat it as no real memory access
        if (data == "--------" || addrHex.size() < 8) {
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

        // Address 0 is valid; do not drop it
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

// ====================== Milestone 3: Cache simulator ======================

struct CacheStats {
    long long accesses         = 0;  // total cache accesses (words)
    long long addresses        = 0;  // unique addresses (instr+data)
    long long hits             = 0;
    long long misses           = 0;
    long long compulsoryMisses = 0;
    long long conflictMisses   = 0;
};

class CacheSim {
public:
    CacheSim(int cacheSizeKB, int blockSize, int associativity,
             const string &policy, int physAddrBits)
        : sizeKB(cacheSizeKB), blockSize(blockSize),
          associativity(associativity), replacementPolicy(policy) {

        long long cacheBytes = (long long)sizeKB * 1024;
        totalBlocks = (int)(cacheBytes / blockSize);
        if (totalBlocks <= 0) {
            numSets = 0;
            offsetBits = indexBits = tagBits = 0;
            indexMask = 0;
            return;
        }

        numSets = totalBlocks / associativity;
        if (numSets <= 0) numSets = 1;

        offsetBits = (int)std::log2(blockSize);
        indexBits  = (int)std::log2(numSets);
        tagBits    = physAddrBits - offsetBits - indexBits;
        if (tagBits < 0) tagBits = 0;

        indexMask = (indexBits > 0) ? ((1ULL << indexBits) - 1ULL) : 0ULL;

        sets.resize(numSets);
        for (int i = 0; i < numSets; ++i) {
            sets[i].lines.resize(associativity);
            sets[i].rrIndex = 0;
            for (int j = 0; j < associativity; ++j) {
                sets[i].lines[j].valid = false;
                sets[i].lines[j].tag   = 0;
            }
        }
    }

    // Returns true on hit, false on miss
    bool access(unsigned long long physAddr) {
        if (numSets == 0) {
            // degenerate cache, treat as always hit
            return true;
        }

        // one logical address seen
        stats.addresses++;

        // base access for this address
        stats.accesses++;

        // Break physical address into tag | index | offset
        unsigned long long addrNoOffset = physAddr >> offsetBits;

        unsigned long long index = 0;
        unsigned long long tag   = 0;

        if (indexBits > 0) {
            index = addrNoOffset & indexMask;
            tag   = addrNoOffset >> indexBits;
        } else {
            index = 0;
            tag   = addrNoOffset;
        }

        Set &set = sets[(size_t)index];

        // HIT check
        for (int i = 0; i < associativity; ++i) {
            if (set.lines[i].valid && set.lines[i].tag == tag) {
                stats.hits++;
                return true;
            }
        }

        // MISS
        stats.misses++;

        // Look for an invalid line first (compulsory miss)
        int freeLine = -1;
        for (int i = 0; i < associativity; ++i) {
            if (!set.lines[i].valid) {
                freeLine = i;
                break;
            }
        }

        // On a miss, we must fetch the entire block from memory.
        // There are (blockSize / 4) 4-byte words in the block.
        // We've already counted 1, so add the rest.
        int wordsPerBlock = blockSize / 4;
        if (wordsPerBlock > 1) {
            stats.accesses += (wordsPerBlock - 1);
        }

        if (freeLine != -1) {
            stats.compulsoryMisses++;
            set.lines[freeLine].valid = true;
            set.lines[freeLine].tag   = tag;
        } else {
            // All valid → conflict miss
            stats.conflictMisses++;
            int victim = set.rrIndex % associativity;
            set.rrIndex = (set.rrIndex + 1) % associativity;
            set.lines[victim].valid = true;
            set.lines[victim].tag   = tag;
        }

        return false;
    }

    // Invalidate any cache block that overlaps this physical page
    void invalidatePage(int physPageNum) {
        if (numSets == 0) return;

        const int PAGE_SIZE = 4096;
        unsigned long long pageBase = (unsigned long long)physPageNum * PAGE_SIZE;
        unsigned long long pageEnd  = pageBase + PAGE_SIZE - 1;

        for (int i = 0; i < numSets; ++i) {
            for (int j = 0; j < associativity; ++j) {
                if (!sets[i].lines[j].valid) continue;

                unsigned long long tag   = sets[i].lines[j].tag;
                unsigned long long block = (tag << indexBits) |
                                           (unsigned long long)i;
                unsigned long long blockAddr = block * blockSize;
                unsigned long long blockEnd  = blockAddr + blockSize - 1;

                if (!(blockEnd < pageBase || blockAddr > pageEnd)) {
                    sets[i].lines[j].valid = false;
                }
            }
        }
    }

    long long countUnusedBlocks() const {
        long long unused = 0;
        for (const auto &set : sets) {
            for (const auto &line : set.lines) {
                if (!line.valid) unused++;
            }
        }
        return unused;
    }

    const CacheStats &getStats() const { return stats; }
    int getTotalBlocks() const { return totalBlocks; }
    int getBlockSize()  const { return blockSize; }
    int getSizeKB()     const { return sizeKB; }

private:
    struct Line {
        bool valid;
        unsigned long long tag;
    };
    struct Set {
        vector<Line> lines;
        int rrIndex;
    };

    int sizeKB;
    int blockSize;
    int associativity;
    string replacementPolicy;

    int totalBlocks = 0;
    int numSets     = 0;
    int offsetBits  = 0;
    int indexBits   = 0;
    int tagBits     = 0;
    unsigned long long indexMask = 0;

    vector<Set> sets;
    CacheStats stats;
};

// ====================== VM access helper (uses cache) ======================

int simulateAccess(int proc,
                   uint32_t virtualAddr,
                   long long &virtualPagesMapped,
                   long long &pageTableHits,
                   long long &pagesFromFree,
                   long long &totalPageFaults,
                   vector<vector<PTE>> &pageTables,
                   vector<PhysPageInfo> &physPages,
                   vector<long long> &usedEntryCount,
                   long long systemPages,
                   int &nextVictimIdx,
                   CacheSim &cache,
                   long long &totalCycles,
                   int /*accessType*/) {
    const int PAGE_SIZE = 4096;

    int vpage = (int)(virtualAddr / PAGE_SIZE);
    if (vpage < 0 || vpage >= (int)pageTables[proc].size())
        return -1;

    virtualPagesMapped++;

    PTE &entry = pageTables[proc][vpage];
    int physPageNum = -1;

    // ---- Page table lookup ----
    if (entry.valid) {
        pageTableHits++;
        physPageNum = entry.physPage;
    } else {
        // Need a physical page, either free or evict
        auto findFreePhysPage = [&](void) -> int {
            for (size_t i = (size_t)systemPages; i < physPages.size(); ++i) {
                if (physPages[i].free)
                    return (int)i;
            }
            return -1;
        };

        auto chooseVictimPage = [&](void) -> int {
            int n = (int)physPages.size();
            if (n <= (int)systemPages) return -1;

            for (int attempts = 0; attempts < n; ++attempts) {
                if (nextVictimIdx < (int)systemPages || nextVictimIdx >= n)
                    nextVictimIdx = (int)systemPages;
                int idx = nextVictimIdx;
                nextVictimIdx++;
                if (nextVictimIdx >= n) nextVictimIdx = (int)systemPages;

                if (!physPages[idx].free && idx >= systemPages)
                    return idx;
            }
            return -1;
        };

        int freeIdx = findFreePhysPage();
        if (freeIdx != -1) {
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
            physPageNum = freeIdx;
        } else {
            // Page fault: pick a victim
            int victimIdx = chooseVictimPage();
            if (victimIdx == -1) return -1;

            // Invalidate victim's cache blocks
            cache.invalidatePage(victimIdx);
            totalCycles += 100; // page fault penalty

            int vProc  = physPages[victimIdx].ownerProc;
            int vVpage = physPages[victimIdx].ownerVPage;
            if (vProc >= 0 && vVpage >= 0) {
                PTE &victimEntry = pageTables[vProc][vVpage];
                victimEntry.valid    = false;
                victimEntry.physPage = -1;
            }

            physPages[victimIdx].ownerProc  = proc;
            physPages[victimIdx].ownerVPage = vpage;

            entry.valid    = true;
            entry.physPage = victimIdx;

            if (!entry.everUsed) {
                entry.everUsed = true;
                usedEntryCount[proc]++;
            }

            totalPageFaults++;
            physPageNum = victimIdx;
        }
    }

    // 2. Physical Address = physPageNum * 4096 + offset
    uint32_t pageOffset = virtualAddr % PAGE_SIZE;
    unsigned long long physAddr =
        (unsigned long long)physPageNum * PAGE_SIZE + pageOffset;

    // 3. Cache access
    bool hit = cache.access(physAddr);

    // Cache timing from PDF:
    // hit: 1 cycle
    // miss: 4 * number_of_4byte_reads_to_fill_block
    if (hit) {
        totalCycles += 1;
    } else {
        int memoryReads = (int)ceil((double)cache.getBlockSize() / 4.0);
        totalCycles += 4 * memoryReads;
    }

    return physPageNum;
}

// ====================== VM + Cache simulation ======================

void runVirtualMemorySim(long long physMemBytes,
                         long long numPhysPages,
                         long long systemPages,
                         int pteBits,
                         int timeSlice,
                         const vector<string> &traceFiles,
                         int cacheSizeKB,
                         int blockSize,
                         int associativity,
                         const string &replacementPolicy,
                         int physAddrBits,
                         long long totalBlocks,
                         long long overheadBytes) {
    (void)replacementPolicy; // we only implement RR in this version
    (void)totalBlocks;

    const int PAGE_SIZE   = 4096;
    const int NUM_VPAGES  = 512 * 1024;

    int numTraces = (int)traceFiles.size();
    if (numTraces == 0) {
        cout << "\n***** VIRTUAL MEMORY SIMULATION RESULTS *****\n\n";
        cout << "No trace files provided.\n";
        return;
    }

    // Physical pages setup
    vector<PhysPageInfo> physPages((size_t)numPhysPages);
    for (long long i = 0; i < numPhysPages; ++i) {
        if (i < systemPages) {
            physPages[(size_t)i].free       = false;
            physPages[(size_t)i].ownerProc  = -1;
            physPages[(size_t)i].ownerVPage = -1;
        } else {
            physPages[(size_t)i].free       = true;
            physPages[(size_t)i].ownerProc  = -1;
            physPages[(size_t)i].ownerVPage = -1;
        }
    }

    // Page tables
    vector<vector<PTE>> pageTables(numTraces,
                                   vector<PTE>(NUM_VPAGES, { -1, false, false }));
    vector<long long> usedEntryCount(numTraces, 0);

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

    // Cache + CPI tracking
    CacheSim cache(cacheSizeKB, blockSize, associativity, "RR", physAddrBits);
    long long totalInstructions = 0;
    long long totalInstrBytes   = 0;
    long long totalSrcBytes     = 0;
    long long totalDstBytes     = 0;
    long long totalCycles       = 0;

    int current       = 0;
    int nextVictimIdx = (int)systemPages;

    // Round-robin over processes
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

            uint32_t eipAddr;
            bool haveEip = parseEipLine(eipLine, eipAddr);

            // EIP access (instruction fetch)
            if (haveEip) {
                totalInstructions++;
                totalInstrBytes += 4; // assume 4-byte instr

                // +2 cycles per instruction (execution)
                totalCycles += 2;

                simulateAccess(current, eipAddr,
                               virtualPagesMapped, pageTableHits,
                               pagesFromFree, totalPageFaults,
                               pageTables, physPages, usedEntryCount,
                               systemPages, nextVictimIdx,
                               cache, totalCycles, 0);
            }

            bool hasDst, hasSrc;
            uint32_t dstAddr = 0, srcAddr = 0;
            parseMemLine(memLine, hasDst, dstAddr, hasSrc, srcAddr);

            // dstM (write)
            if (hasDst) {
                totalDstBytes += 4;
                totalCycles += 1; // +1 to calc effective address

                simulateAccess(current, dstAddr,
                               virtualPagesMapped, pageTableHits,
                               pagesFromFree, totalPageFaults,
                               pageTables, physPages, usedEntryCount,
                               systemPages, nextVictimIdx,
                               cache, totalCycles, 2);
            }

            // srcM (read)
            if (hasSrc) {
                totalSrcBytes += 4;
                totalCycles += 1; // +1 to calc effective address

                simulateAccess(current, srcAddr,
                               virtualPagesMapped, pageTableHits,
                               pagesFromFree, totalPageFaults,
                               pageTables, physPages, usedEntryCount,
                               systemPages, nextVictimIdx,
                               cache, totalCycles, 1);
            }

            instrCount++;
        }

        current = (current + 1) % numTraces;
    }

    for (int i = 0; i < numTraces; ++i) {
        if (procs[i].file.is_open())
            procs[i].file.close();
    }

    // ===== Milestone 2 results =====
    cout << "\n***** VIRTUAL MEMORY SIMULATION RESULTS *****\n" << endl;
    cout << "Physical Pages Used By SYSTEM:\t" << systemPages << endl;
    cout << "Pages Available to User:\t"    << (numPhysPages - systemPages)   << endl;

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

    // ===== Milestone 3: Cache results =====
    const CacheStats &cs = cache.getStats();

    cout << "***** CACHE SIMULATION RESULTS *****\n";
    cout << "Total Cache Accesses:\t" << cs.accesses
         << "  (" << cs.addresses << " addresses)\n";
    cout << "--- Instruction Bytes:\t" << totalInstrBytes << "\n";
    cout << "--- SrcDst Bytes:\t" << (totalSrcBytes + totalDstBytes) << "\n";
    cout << "Cache Hits:\t\t" << cs.hits << "\n";
    cout << "Cache Misses:\t\t" << cs.misses << "\n";
    cout << "--- Compulsory Misses:\t" << cs.compulsoryMisses << "\n";
    cout << "--- Conflict Misses:\t" << cs.conflictMisses << "\n";
    cout << "***** ***** CACHE HIT & MISS RATE: ***** *****\n";

    double hitRate  = 0.0;
    double missRate = 0.0;
    // hits/misses are counted per address
    if (cs.addresses > 0) {
        hitRate  = (cs.hits   * 100.0) / cs.addresses;
        missRate = (cs.misses * 100.0) / cs.addresses;
    }

    cout << "Hit Rate:\t\t" << fixed << setprecision(4) << hitRate  << "%\n";
    cout << "Miss Rate:\t\t" << fixed << setprecision(4) << missRate << "%\n";

    double cpi = 0.0;
    if (totalInstructions > 0) {
        cpi = (double)totalCycles / (double)totalInstructions;
    }
    cout << "CPI:\t\t\t" << fixed << setprecision(2)
         << cpi << " Cycles/Instruction (" << totalCycles << ")\n";

    // Unused cache space estimate, using implementation memory (data+overhead)
    long long unusedBlocks  = cache.countUnusedBlocks();
    long long totalBlocksLL = cache.getTotalBlocks();

    double implMemKB = (cache.getSizeKB() * 1024.0 + overheadBytes) / 1024.0;

    double fracUnused = 0.0;
    if (totalBlocksLL > 0) {
        fracUnused = (double)unusedBlocks / (double)totalBlocksLL;
    }

    double unusedKB = implMemKB * fracUnused;
    double totalKB  = implMemKB;
    double wastePct = 0.0;
    if (totalKB > 0.0) {
        wastePct = (unusedKB * 100.0) / totalKB;
    }

    double wasteCost = unusedKB * 0.07;  // $0.07 per KB

    cout << "Unused Cache Space:\t" << fixed << setprecision(2)
         << unusedKB << " KB / " << totalKB << " KB = "
         << wastePct << "%  Waste: $" << wasteCost << "\n";
    cout << "Unused Cache Blocks:\t" << unusedBlocks
         << " / " << totalBlocksLL << "\n";
}

// ====================== main (M1 + M2 + M3) ======================

int main(int argc, char* argv[]) {
    if (argc < 3) {
        cerr << "Usage: " << argv[0]
             << " -s <cacheSizeKB> -b <blockSize> -a <associativity> -r <RR|RND> trace1.trc [trace2.trc] [trace3.trc]\n";
        return 1;
    }

    // Milestone 1/2 system parameters (match professor's example)
    int physMemory   = 128;    // MB
    double percentUsed = 75.0; // %
    int timeSlice    = -1;     // ALL instructions

    // Cache parameters (from command line)
    int cacheSize      = -1;        // KB
    int blockSize      = -1;        // bytes
    int associativity  = -1;
    string replacementPolicy = "RR";

    vector<string> traceFiles;

    // ---- Parse command-line arguments ----
    for (int i = 1; i < argc; ++i) {
        string arg = argv[i];

        if (arg == "-s") {
            if (i + 1 >= argc) { cerr << "ERROR: -s requires a value\n"; return 1; }
            cacheSize = stoi(argv[++i]);
        }
        else if (arg == "-b") {
            if (i + 1 >= argc) { cerr << "ERROR: -b requires a value\n"; return 1; }
            blockSize = stoi(argv[++i]);
        }
        else if (arg == "-a") {
            if (i + 1 >= argc) { cerr << "ERROR: -a requires a value\n"; return 1; }
            associativity = stoi(argv[++i]);
        }
        else if (arg == "-r") {
            if (i + 1 >= argc) { cerr << "ERROR: -r requires a value\n"; return 1; }
            replacementPolicy = argv[++i]; // "RR" or "RND"
        }
        else if (!arg.empty() && arg[0] == '-') {
            cerr << "ERROR: Unknown option: " << arg << "\n";
            return 1;
        }
        else {
            // treat as trace file
            traceFiles.push_back(arg);
        }
    }

    // ---- Validate required parameters ----
    if (cacheSize <= 0 || blockSize <= 0 || associativity <= 0) {
        cerr << "ERROR: Missing or invalid -s, -b, or -a value.\n";
        return 1;
    }
    if (traceFiles.empty()) {
        cerr << "ERROR: No trace files provided.\n";
        return 1;
    }

    const int PAGE_SIZE = 4096;

    long long cacheBytes   = (long long)cacheSize * 1024;
    long long physMemBytes = (long long)physMemory * 1024 * 1024;

    long long totalBlocks = cacheBytes / blockSize;
    long long rows        = totalBlocks / associativity;

    int offsetBits   = (int)log2(blockSize);
    int indexBits    = (int)log2(rows);
    int physAddrBits = (int)log2(physMemBytes);
    int tagBits      = physAddrBits - indexBits - offsetBits;

    long long overheadBytes = (long long)(rows * associativity * (tagBits + 1)) / 8;
    long long implMemBytes  = cacheBytes + overheadBytes;
    double implMemKB        = implMemBytes / 1024.0;
    double cost             = implMemKB * 0.07;

    long long numPhysPages   = physMemBytes / PAGE_SIZE;
    long long systemPages    = (long long)(numPhysPages * (percentUsed / 100.0));
    int pteBits              = 1 + (int)log2(numPhysPages);
    long long totalPageTableRAM =
        (long long)((512.0 * 1024 * 3 * pteBits) / 8.0);

    // ---- Milestone 1 output ----
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
    cout << "Instructions / Time Slice:\t";
    if (timeSlice < 0) cout << "ALL" << endl;
    else               cout << timeSlice << endl;

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

    // ---- Milestone 2 + 3 ----
    runVirtualMemorySim(physMemBytes, numPhysPages, systemPages,
                        pteBits, timeSlice, traceFiles,
                        cacheSize, blockSize, associativity,
                        replacementPolicy, physAddrBits,
                        totalBlocks, overheadBytes);

    return 0;
}
