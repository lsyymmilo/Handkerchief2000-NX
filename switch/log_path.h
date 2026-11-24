// Forced include (-include build_hk.py) for every translation unit.
// SH3_LOG_PATH must be a plain string literal: passing it as a -D on the command
// line expanded to a literal whose CONTENT contained quote characters, and FAT
// forbids '"' in file names, so fopen(log) failed on every run (zero logs).
#define SH3_LOG_PATH "sdmc:/switch/Hdk2000/hklog.txt"
