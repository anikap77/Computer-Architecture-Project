#include <iostream>
#include <iomanip>
#include <string>
#include <fstream>
#include <vector>
#include <sstream>
#include <cmath>
#include <cstdint>
#include <algorithm> // For random

using namespace std;

// ====================== Cache Structures & Classes ======================

struct CacheConfig {
    int sizeKB;
    int blockSize;
    int associativity;
    string replPolicy; // "RR" or "RND"
};

struct CacheLine {
    bool valid;
    uint32_t tag;
    // You might need a sequence number here for Round Robin if not using a global counter
    // int lastUsed; 
};

struct CacheSet {
    vector<CacheLine> lines;
    int rrCounter; // For Round Robin replacement
};

struct CacheStats {
    long long accesses;
    long long hits;
    long long misses;
    long long compulsoryMisses;
    long long conflictMisses;
    long long totalCycles;
};

class CacheSimulator {
public:
    vector<CacheSet> sets;
    CacheConfig config;
    CacheStats stats;
    
    int indexBits;
    int offsetBits;
    int tagBits;
    long long numSets;

    CacheSimulator(CacheConfig cfg, long long physMemSize) {
        config = cfg;
        // Initialize stats
        stats = {0, 0, 0, 0, 0, 0};

        long long cacheBytes = (long long)cfg.sizeKB * 1024;
        long long numBlocks = cacheBytes / cfg.blockSize;
        numSets = numBlocks / cfg.associativity;

        // Calculate bit widths
        offsetBits = (int)log2(cfg.blockSize);
        indexBits = (int)log2(numSets);
        // Tag bits depends on Physical Memory size (from M1 calc)
        int physAddrBits = (int)log2(physMemSize); 
        tagBits = physAddrBits - indexBits - offsetBits;

        // Initialize sets
        sets.resize(numSets);
        for (int i = 0; i < numSets; i++) {
            sets[i].lines.resize(cfg.associativity, {false, 0});
            sets[i].rrCounter = 0;
        }
    }

    // Main function to access cache
    // type: 0 = Instruction Fetch, 1 = Data Read, 2 = Data Write
    void access(uint32_t physAddr, int type) {
        stats.accesses++;
        
        // 1. Calculate Tag and Index from physAddr
        // TODO: Implement bitwise shifting/masking to get tag and index
        // uint32_t offset = ...
        // uint32_t index = ...
        // uint32_t tag = ...
        
        // Note: For now, hardcoding 0. Gotta replace this.
        uint32_t index = 0; 
        uint32_t tag = 0;

        // 2. Check for HIT
        bool hit = false;
        // TODO: Iterate through sets[index].lines to find if tag matches and valid is true
        
        if (hit) {
            stats.hits++;
            stats.totalCycles += 1; // Cache hit cost
        } else {
            stats.misses++;
            // Cost calculation per PDF Page 7
            int memoryReads = (int)ceil((double)config.blockSize / 4.0);
            stats.totalCycles += (4 * memoryReads);

            // 3. Handle MISS (Placement)
            // TODO: Look for an invalid line (Compulsory Miss)
            // If found, place tag there. 
            // stats.compulsoryMisses++;

            // TODO: If no invalid line, Evict (Conflict Miss)
            // Use config.replPolicy to choose victim.
            // If "RR", use sets[index].rrCounter, then increment mod associativity
            // stats.conflictMisses++;
        }

        // Add execution overhead cycles
        if (type == 0) stats.totalCycles += 0; // Instruction fetch overhead handled by main loop (+2)
        // Note: The PDF implies specific overheads per instruction type.
        // Usually, we add the base memory access cost here.
    }

    // REQUIRED by PDF Page 6: Invalidate blocks belonging to a specific physical page
    void invalidatePage(int physPageNum) {
        // Physical Page Size is 4096 bytes.
        // A single physical page contains many cache blocks.
        // We must iterate over the specific range of addresses in this page
        // OR iterate over the whole cache and check if the tag matches this page.
        
        // Optimization: It is usually faster to iterate through the cache 
        // because reconstruction of address from Tag+Index is easier.
        
        for (int i = 0; i < numSets; i++) {
            for (int j = 0; j < config.associativity; j++) {
                if (sets[i].lines[j].valid) {
                    // TODO: Reconstruct Physical Address from sets[i].lines[j].tag and i (index)
                    // uint32_t reconstructedAddr = ...
                    
                    // TODO: Extract Page Number (addr >> 12)
                    // if (pageNumber == physPageNum) {
                    //     sets[i].lines[j].valid = false;
                    // }
                }
            }
        }
    }
    
    // Helper to calculate unused space for the report
    long long countUnusedBlocks() {
        long long unused = 0;
        for(const auto& set : sets) {
            for(const auto& line : set.lines) {
                if(!line.valid) unused++;
            }
        }
        return unused;
    }
};

// ====================== Existing Milestone 2 Helpers ======================

struct PTE {
    int  physPage;
    bool valid;
    bool everUsed;
};

struct PhysPageInfo {
    bool free;
    int  ownerProc;
    int  ownerVPage;
};

struct ProcessInfo {
    ifstream file;
    bool done;
    string filename;
};

// ... (Keep existing parseEipLine, parseMemLine, trim, findFreePhysPage, chooseVictimPage) ...
// Copy-paste any existing helper functions here or include them.
static inline string trim(const string &s) {
    size_t b = s.find_first_not_of(" \t\r\n");
    if (b == string::npos) return "";
    size_t e = s.find_last_not_of(" \t\r\n");
    return s.substr(b, e - b + 1);
}

bool parseEipLine(const string &line, uint32_t &addrOut) {
    size_t colon = line.find(':');
    if (colon == string::npos) return false;
    string after = trim(line.substr(colon + 1));
    if (after.empty()) return false;
    stringstream ss(after);
    string token;
    if (!(ss >> token)) return false;
    if (token.size() < 8) return false;
    string addrHex = token.substr(token.size() - 8);
    if (addrHex.find_first_not_of("0123456789abcdefABCDEF") != string::npos) return false;
    stringstream hx;
    hx << std::hex << addrHex;
    hx >> addrOut;
    return true;
}

void parseMemLine(const string &line, bool &hasDst, uint32_t &dstAddr, bool &hasSrc, uint32_t &srcAddr) {
    hasDst = false; hasSrc = false;
    string s = line;
    size_t posDst = s.find("dstM:");
    size_t posSrc = s.find("srcM:");
    auto parseOne = [](const string &sub, uint32_t &addrOut, bool &hasAddr) {
        stringstream ss(sub);
        string label, addrHex, data;
        if (!(ss >> label >> addrHex >> data)) { hasAddr = false; return; }
        if (data == "--------" || addrHex.size() < 8) { hasAddr = false; return; }
        string addr8 = addrHex.substr(addrHex.size() - 8);
        if (addr8.find_first_not_of("0123456789abcdefABCDEF") != string::npos) { hasAddr = false; return; }
        stringstream hx; hx << std::hex << addr8; hx >> addrOut;
        if (addrOut == 0) { hasAddr = false; return; }
        hasAddr = true;
    };
    if (posDst != string::npos) {
        size_t end = (posSrc != string::npos) ? posSrc : string::npos;
        parseOne(trim(s.substr(posDst, end - posDst)), dstAddr, hasDst);
    }
    if (posSrc != string::npos) {
        parseOne(trim(s.substr(posSrc)), srcAddr, hasSrc);
    }
}

int findFreePhysPage(vector<PhysPageInfo> &physPages, long long systemPages) {
    for (size_t i = (size_t)systemPages; i < physPages.size(); ++i) {
        if (physPages[i].free) return (int)i;
    }
    return -1;
}

int chooseVictimPage(vector<PhysPageInfo> &physPages, long long systemPages, int &nextVictim) {
    int n = (int)physPages.size();
    if (n <= (int)systemPages) return -1;
    for (int attempts = 0; attempts < n; ++attempts) {
        if (nextVictim < (int)systemPages || nextVictim >= n) nextVictim = (int)systemPages;
        int idx = nextVictim;
        nextVictim++;
        if (nextVictim >= n) nextVictim = (int)systemPages;
        if (!physPages[idx].free && idx >= systemPages) return idx;
    }
    return -1;
}

// ====================== Integrated Simulation Function ======================

void simulateAccess(int proc,
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
                    CacheSimulator &cache, // <--- PASSED BY REFERENCE
                    int accessType) {      // <--- NEW PARAM: 0=Instr, 1=Read, 2=Write
    
    const int PAGE_SIZE = 4096;
    if (virtualAddr == 0) return;

    int vpage = (int)(virtualAddr / PAGE_SIZE);
    if (vpage < 0 || vpage >= (int)pageTables[proc].size()) return;

    // 1. Virtual Memory Translation
    virtualPagesMapped++; // Note: This metric from M2 might be interpreted as "Accesses" now
    
    PTE &entry = pageTables[proc][vpage];
    int physPageNum = -1;

    if (entry.valid) {
        pageTableHits++;
        physPageNum = entry.physPage;
    } else {
        // Handle Miss
        int freeIdx = findFreePhysPage(physPages, systemPages);
        if (freeIdx != -1) {
            // Found Free
            physPages[freeIdx].free = false;
            physPages[freeIdx].ownerProc = proc;
            physPages[freeIdx].ownerVPage = vpage;
            entry.valid = true;
            entry.physPage = freeIdx;
            physPageNum = freeIdx;
            if (!entry.everUsed) { entry.everUsed = true; usedEntryCount[proc]++; }
            pagesFromFree++;
        } else {
            // Page Fault - Eviction Needed
            int victimIdx = chooseVictimPage(physPages, systemPages, nextVictimIdx);
            if (victimIdx == -1) return; // Should not happen

            // *** CACHE INTEGRATION ***
            // Before remapping, we MUST invalidate cache entries for the victim page
            // Per PDF Page 6: "Once you have the physical address... Invalidate any cache blocks"
            cache.invalidatePage(victimIdx); 
            // *************************
            
            // Add Page Fault Cost to cycles
            cache.stats.totalCycles += 100; 

            // Unmap victim info in their Page Table
            int vProc = physPages[victimIdx].ownerProc;
            int vVpage = physPages[victimIdx].ownerVPage;
            if (vProc >= 0 && vVpage >= 0) {
                pageTables[vProc][vVpage].valid = false;
                pageTables[vProc][vVpage].physPage = -1;
            }

            // Remap to current process
            physPages[victimIdx].ownerProc = proc;
            physPages[victimIdx].ownerVPage = vpage;
            entry.valid = true;
            entry.physPage = victimIdx;
            physPageNum = victimIdx;
            if (!entry.everUsed) { entry.everUsed = true; usedEntryCount[proc]++; }
            totalPageFaults++;
        }
    }

    // 2. Physical Address Calculation
    // PA = (PhysicalPageNum * 4096) + (Offset)
    uint32_t pageOffset = virtualAddr % PAGE_SIZE;
    uint32_t physAddr = (uint32_t)((physPageNum << 12) | pageOffset);

    // 3. Perform Cache Access
    cache.access(physAddr, accessType);
}

// ... (keep runVirtualMemorySim structure, but modify calls) ...

void runFullSimulation(long long physMemBytes,
                        long long numPhysPages,
                        long long systemPages,
                        int pteBits,
                        int timeSlice,
                        const vector<string> &traceFiles,
                        CacheConfig cacheCfg) { // <--- Receive Cache Config
    
    // Initialize Cache Simulator
    CacheSimulator cache(cacheCfg, physMemBytes);

    const int PAGE_SIZE = 4096;
    const int NUM_VPAGES = 512 * 1024;
    int numTraces = (int)traceFiles.size();
    
    // ... (Init PhysPages, PageTables as in M2) ...
    vector<PhysPageInfo> physPages((size_t)numPhysPages);
    for (long long i = 0; i < numPhysPages; ++i) {
         // ... (Same M2 logic for free/system) ...
         physPages[i].free = (i >= systemPages);
         physPages[i].ownerProc = -1; physPages[i].ownerVPage = -1;
    }

    vector<vector<PTE>> pageTables(numTraces, vector<PTE>(NUM_VPAGES, { -1, false, false }));
    vector<long long> usedEntryCount(numTraces, 0);

    // Stats
    long long virtualPagesMapped = 0;
    long long pageTableHits = 0;
    long long pagesFromFree = 0;
    long long totalPageFaults = 0;

    // ... (File opening logic same as M2) ...
    vector<ProcessInfo> procs(numTraces);
    int active = 0;
    for(int i=0; i<numTraces; ++i) {
        procs[i].filename = traceFiles[i];
        procs[i].file.open(traceFiles[i].c_str());
        if(procs[i].file.is_open()) active++;
    }

    int current = 0;
    int nextVictimIdx = (int)systemPages;

    // Loop
    while (active > 0 && numPhysPages > systemPages) {
        if (procs[current].done) { current = (current + 1) % numTraces; continue; }

        int instrCount = 0;
        while ((timeSlice == -1 || instrCount < timeSlice) && !procs[current].done) {
            string eipLine, memLine;
            if (!getline(procs[current].file, eipLine)) { procs[current].done = true; active--; break; }
            if (!getline(procs[current].file, memLine)) { procs[current].done = true; active--; break; }

            eipLine = trim(eipLine);
            memLine = trim(memLine);

            // EIP (Instruction Fetch) - Type 0
            uint32_t eipAddr;
            if (parseEipLine(eipLine, eipAddr)) {
                cache.stats.totalCycles += 2; // +2 cycles to execute instruction (Page 7)
                simulateAccess(current, eipAddr, virtualPagesMapped, pageTableHits, pagesFromFree, totalPageFaults,
                               pageTables, physPages, usedEntryCount, systemPages, nextVictimIdx, 
                               cache, 0);
            }

            bool hasDst, hasSrc;
            uint32_t dstAddr = 0, srcAddr = 0;
            parseMemLine(memLine, hasDst, dstAddr, hasSrc, srcAddr);

            // Src (Data Read) - Type 1
            if (hasSrc) {
                cache.stats.totalCycles += 1; // +1 cycle calc effective addr (Page 7)
                simulateAccess(current, srcAddr, virtualPagesMapped, pageTableHits, pagesFromFree, totalPageFaults,
                               pageTables, physPages, usedEntryCount, systemPages, nextVictimIdx, 
                               cache, 1);
            }
            
            // Dst (Data Write) - Type 2
            if (hasDst) {
                cache.stats.totalCycles += 1; // +1 cycle calc effective addr (Page 7)
                simulateAccess(current, dstAddr, virtualPagesMapped, pageTableHits, pagesFromFree, totalPageFaults,
                               pageTables, physPages, usedEntryCount, systemPages, nextVictimIdx, 
                               cache, 2);
            }

            instrCount++;
        }
        current = (current + 1) % numTraces;
    }

    // Close files...
    
    // ===== PRINT M2 RESULTS (Same as before) =====
    // ... (Your M2 Print logic here) ...

    // ===== PRINT M3 CACHE RESULTS =====
    cout << "\n\n***** CACHE SIMULATION RESULTS *****\n" << endl;
    cout << "Total Cache Accesses:\t\t" << cache.stats.accesses << endl;
    cout << "Cache Hits:\t\t\t" << cache.stats.hits << endl;
    cout << "Cache Misses:\t\t\t" << cache.stats.misses << endl;
    cout << "--- Compulsory Misses:\t\t" << cache.stats.compulsoryMisses << endl;
    cout << "--- Conflict Misses:\t\t" << cache.stats.conflictMisses << endl;
    
    cout << "\n***** CACHE HIT & MISS RATE: *****\n" << endl;
    
    double hitRate = (cache.stats.accesses > 0) ? (double)cache.stats.hits / cache.stats.accesses * 100.0 : 0.0;
    double missRate = 100.0 - hitRate;
    
    // CPI Calculation: Total Cycles / Total Instructions
    // Note: You need to track total instructions executed separately in the loop to get exact CPI
    // For now, assuming virtualPagesMapped approximates accesses, but you should count `eipAddr` parses.
    
    cout << fixed << setprecision(4);
    cout << "Hit Rate:\t\t\t" << hitRate << "%" << endl;
    cout << "Miss Rate:\t\t\t" << missRate << "%" << endl;
    cout << "CPI:\t\t\t\t" << (double)cache.stats.totalCycles / cache.stats.accesses << " Cycles/Access (approx)" << endl; 
    // ^ TODO: Fix denominator to be # Instructions for proper CPI

    long long unusedBlocks = cache.countUnusedBlocks();
    double unusedKB = (double)(unusedBlocks * cacheCfg.blockSize + (unusedBlocks * (cache.tagBits+1)/8)) / 1024.0;

    cout << "Unused Cache Space:\t\t" << unusedKB << " KB / " << cacheCfg.sizeKB << " KB" << endl;
    cout << "Unused Cache Blocks:\t\t" << unusedBlocks << " / " << (cacheCfg.sizeKB * 1024 / cacheCfg.blockSize) << endl;
}

// ====================== Main ======================

int main(int argc, char* argv[]) {
    // ... (Arg Parsing - Note: Gotta update this to parse -s, -b, -a, -r) ...
    // ... (Params are hardcoded params for now) ...

    int cacheSize = 512;
    int blockSize = 16;
    int associativity = 4;
    string replacementPolicy = "Round Robin";
    int physMemory = 1024;
    double percentUsed = 75.0;
    int timeSlice = 100;
    
    vector<string> traceFiles;
    for (int i = 1; i < argc; ++i) traceFiles.push_back(argv[i]); // Simplistic

    long long physMemBytes = (long long)physMemory * 1024 * 1024;
    long long numPhysPages = physMemBytes / 4096;
    long long systemPages = (long long)(numPhysPages * (percentUsed / 100.0));
    int pteBits = 1 + (int)log2(numPhysPages);
    
    CacheConfig cacheCfg;
    cacheCfg.sizeKB = cacheSize;
    cacheCfg.blockSize = blockSize;
    cacheCfg.associativity = associativity;
    cacheCfg.replPolicy = replacementPolicy;

    // ... (M1 Print Logic) ...

    // Run Full M2 + M3 Sim
    runFullSimulation(physMemBytes, numPhysPages, systemPages, pteBits, timeSlice, traceFiles, cacheCfg);

    return 0;
}
