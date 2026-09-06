#ifndef NVIMMESSAGEPACK_H
#define NVIMMESSAGEPACK_H
#include <QVariant>
#include <QByteArray>
#include <QtEndian>
#include <cstring>

// Small MessagePack codec for the Neovim RPC transport. No socket or server.
namespace NvimMessagePack {
inline void number(QByteArray& out, quint64 n, int size)
{
    for(int i = size - 1; i >= 0; --i) out.append(char(n >> (i * 8)));
}
inline QByteArray encode(const QVariant& v)
{
    QByteArray out;
    if(!v.isValid()) return QByteArray(1, char(0xc0));
    switch(v.type()) {
    case QVariant::Bool: return QByteArray(1, v.toBool() ? char(0xc3) : char(0xc2));
    case QVariant::Int: case QVariant::UInt: case QVariant::LongLong: case QVariant::ULongLong:
        out.append(char(0xd3)); number(out, quint64(v.toLongLong()), 8); break;
    case QVariant::List: case QVariant::StringList: {
        const auto list = v.toList(); out.append(char(0xdd)); number(out, list.size(), 4);
        for(const auto& item : list) out += encode(item);
        break;
    }
    case QVariant::Map: {
        const auto map = v.toMap(); out.append(char(0xdf)); number(out, map.size(), 4);
        for(auto i = map.begin(); i != map.end(); ++i) { out += encode(i.key()); out += encode(i.value()); }
        break;
    }
    default: {
        const auto bytes = v.type() == QVariant::ByteArray ? v.toByteArray() : v.toString().toUtf8();
        out.append(char(v.type() == QVariant::ByteArray ? 0xc6 : 0xdb)); number(out, bytes.size(), 4); out += bytes;
    }
    }
    return out;
}
class Reader {
public:
    explicit Reader(const QByteArray& bytes) : data(bytes) {}
    int pos = 0;
    bool complete = true;
    bool invalid = false;
    quint64 read(int size) {
        if(size < 0 || data.size() - pos < size) { complete = false; return 0; }
        quint64 value = 0;
        for(int i = 0; i < size; ++i) value = (value << 8) | quint8(data.at(pos++));
        return value;
    }
    QVariant value(int depth = 0) {
        if(depth > 64) { invalid = true; return {}; }
        const quint8 tag = read(1);
        if(!complete) return {};
        if(tag <= 0x7f) return int(tag);
        if(tag >= 0xe0) return int(qint8(tag));
        if((tag & 0xe0) == 0xa0) return string(tag & 31, false);
        if((tag & 0xf0) == 0x90) return array(tag & 15, depth);
        if((tag & 0xf0) == 0x80) return map(tag & 15, depth);
        switch(tag) {
        case 0xc0: return {};
        case 0xc2: return false;
        case 0xc3: return true;
        case 0xcc: return int(read(1));
        case 0xcd: return int(read(2));
        case 0xce: return QVariant::fromValue(qulonglong(read(4)));
        case 0xcf: return QVariant::fromValue(qulonglong(read(8)));
        case 0xd0: return int(qint8(read(1)));
        case 0xd1: return int(qint16(read(2)));
        case 0xd2: return int(qint32(read(4)));
        case 0xd3: return QVariant::fromValue(qlonglong(read(8)));
        case 0xca: { quint32 bits = read(4); float n; std::memcpy(&n, &bits, 4); return double(n); }
        case 0xcb: { quint64 bits = read(8); double n; std::memcpy(&n, &bits, 8); return n; }
        case 0xd9: return string(read(1), false);
        case 0xda: return string(read(2), false);
        case 0xdb: return string(read(4), false);
        case 0xc4: return string(read(1), true);
        case 0xc5: return string(read(2), true);
        case 0xc6: return string(read(4), true);
        case 0xdc: return array(read(2), depth);
        case 0xdd: return array(read(4), depth);
        case 0xde: return map(read(2), depth);
        case 0xdf: return map(read(4), depth);
        case 0xd4: case 0xd5: case 0xd6: case 0xd7: case 0xd8:
            read(1); return string(1u << (tag - 0xd4), true);
        case 0xc7: case 0xc8: case 0xc9: {
            const auto length = read(1 << (tag - 0xc7)); read(1); return string(length, true);
        }
        default: invalid = true; return {};
        }
    }
private:
    const QByteArray& data;
    QVariant string(quint64 length, bool binary) {
        if(!complete) return {};
        if(length > quint64(data.size() - pos)) { complete = false; return {}; }
        const auto s = data.mid(pos, int(length)); pos += int(length);
        return binary ? QVariant(s) : QVariant(QString::fromUtf8(s));
    }
    QVariant array(quint64 count, int depth) {
        if(count > 10000000) { invalid = true; return {}; }
        QVariantList list;
        for(quint64 i = 0; i < count && complete && !invalid; ++i) list.append(value(depth + 1));
        return list;
    }
    QVariant map(quint64 count, int depth) {
        if(count > 10000000) { invalid = true; return {}; }
        QVariantMap result;
        for(quint64 i = 0; i < count && complete && !invalid; ++i) {
            const auto key = value(depth + 1).toString(); result.insert(key, value(depth + 1));
        }
        return result;
    }
};
}
#endif
