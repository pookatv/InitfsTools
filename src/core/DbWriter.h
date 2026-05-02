#pragma once
#include <ostream>
#include <vector>
#include <string>
#include "NativeWriter.h"
#include "DbObject.h"
#include "DbReader.h"

class DbWriter : public NativeWriter
{
public:
    explicit DbWriter(std::ostream& stream);

    virtual void write(DbObjectPtr obj);
    virtual void write(const std::vector<uint8_t>& rawBytes);

private:
    std::vector<uint8_t> writeDbValue(const std::string& name,
        const DbValue& value);
    static DbType getDbType(const DbValue& value);
};