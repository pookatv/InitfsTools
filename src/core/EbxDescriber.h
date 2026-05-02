#pragma once
#include <cstdint>
#include <string>
#include <vector>

// Parses the binary metadata block and emits a human-readable text dump
class EbxDescriber
{
public:
    // Returns a human-readable description of the EBX file, or an error string
    static std::string describe(const uint8_t* data, size_t size);
};