// SPDX-License-Identifier: AGPL-3.0-or-later
// Portable wire/parser core shared by the ESP32 sketch and sanitizer tests.
#pragma once
#include <stddef.h>
#include <stdint.h>
#include <string.h>

namespace vmhud {
static const uint8_t kDefaultUid[16] = {0xf7,0xfd,0x15,0x75,0x02,0xda,0x52,0x8e,
                                      0xf0,0xfc,0x31,0x39,0x72,0x2c,0xe9,0xe2};
using Checksum = bool (*)(const uint8_t *, size_t, const uint8_t *, uint8_t *);
inline uint16_t be16(const uint8_t *p) { return (uint16_t(p[0]) << 8) | p[1]; }
inline uint32_t be32(const uint8_t *p) {
    return (uint32_t(p[0]) << 24) | (uint32_t(p[1]) << 16) | (uint32_t(p[2]) << 8) | p[3];
}
inline void put32(uint8_t *p, uint32_t x) {
    p[0]=x>>24; p[1]=x>>16; p[2]=x>>8; p[3]=x;
}
inline uint16_t crc16(const uint8_t *p, size_t n) {
    uint16_t crc=0xffff;
    while (n--) { crc ^= *p++; for (unsigned i=0;i<8;i++) crc=(crc>>1)^((crc&1)?0xa001:0); }
    return crc;
}
// New frame is payload+21 bytes, not payload+29. The two 4-byte sums are
// MD5(header || payload || UID), summed as four big-endian words modulo 2^32.
inline size_t build(uint8_t cmd, const uint8_t *p, size_t n, uint32_t timestamp,
                    uint8_t *out, size_t cap, Checksum sum, bool legacy=false) {
    const size_t overhead=legacy?11:21;
    if (cap<overhead || n>cap-overhead || n>UINT32_MAX-13 || (n && !p)) return 0;
    out[0]=0xa5;out[1]=0x5a;out[2]=0x37;out[3]=legacy?0xc5:0xc3;
    put32(out+4,uint32_t(n)+(legacy?5:13));
    const size_t header=legacy?9:13;
    if (legacy) out[8]=cmd;
    else { put32(out+8,timestamp);out[12]=cmd; }
    if (n) memcpy(out+header,p,n);
    if (legacy) { uint16_t crc=crc16(out+4,n+5);out[header+n]=crc>>8;out[header+n+1]=crc; }
    else {
        const uint8_t uid[16]={0};
        if (!sum || !sum(out,header+n,uid,out+header+n) ||
            !sum(out,header+n,kDefaultUid,out+header+n+4)) return 0;
    }
    return n+overhead;
}

struct Frame {
    const uint8_t *payload=nullptr;
    size_t size=0;
    uint32_t timestamp=0;
    uint8_t command=0;
    bool legacy=false;
};
inline bool parse(const uint8_t *p, size_t n, Checksum sum, Frame &frame) {
    if (n<8 || p[0]!=0xa5 || p[1]!=0x5a || p[2]!=0x37 || (p[3]!=0xc3 && p[3]!=0xc5)) return false;
    const bool legacy=p[3]==0xc5;
    const uint32_t length=be32(p+4);
    if (length<(legacy?5u:13u) || n<(legacy?11u:21u) || size_t(length)!=n-(legacy?6:8)) return false;
    if (legacy) { if (crc16(p+4,n-6)!=be16(p+n-2)) return false; }
    else {
        uint8_t expected[4];
        if (!sum || !sum(p,n-8,kDefaultUid,expected) || memcmp(expected,p+n-4,4)) return false;
    }
    frame.legacy=legacy;
    frame.command=p[legacy?8:12];
    frame.timestamp=legacy?0:be32(p+8);
    frame.payload=p+(legacy?9:13);
    frame.size=n-(legacy?11:21);
    return true; // Runtime UID is unknown; only default checksum/legacy CRC verified.
}

template<size_t Capacity> class Receiver {
    static_assert(Capacity>=21,"receiver must fit a minimum new frame");
public:
    using Handler=void (*)(const Frame &, void *);
    size_t dropped=0;
    void reset() { used=0; }
    void feed(const uint8_t *data, size_t size, Checksum sum, Handler handle, void *user) {
        while (size) {
            size_t take=Capacity-used;
            if (take>size) take=size;
            if (!take) { discard(1);++dropped;continue; }
            memcpy(buffer+used,data,take);used+=take;data+=take;size-=take;
            drain(sum,handle,user);
        }
    }
private:
    uint8_t buffer[Capacity];
    size_t used=0;
    static bool magic(const uint8_t *p) {
        return p[0]==0xa5 && p[1]==0x5a && p[2]==0x37 && (p[3]==0xc3 || p[3]==0xc5);
    }
    void discard(size_t n) { memmove(buffer,buffer+n,used-n);used-=n; }
    void drain(Checksum sum, Handler handle, void *user) {
        while (used>=4) {
            if (!magic(buffer)) {
                size_t skip=1;
                while (skip+4<=used && !magic(buffer+skip)) ++skip;
                discard(skip);continue;
            }
            if (used<8) return;
            const bool legacy=buffer[3]==0xc5;
            const uint32_t length=be32(buffer+4);
            const size_t extra=legacy?6:8;
            if (length<(legacy?5u:13u) || length>Capacity-extra) { discard(1);++dropped;continue; }
            const size_t total=size_t(length)+extra;
            if (used<total) return;
            Frame frame;
            if (!parse(buffer,total,sum,frame)) { discard(1);++dropped;continue; }
            handle(frame,user);
            discard(total);
        }
    }
};

struct Nav {
    uint8_t direction=0, minutes=0, timeThird=0;
    uint16_t hours=0, speed=0;
    uint32_t distance=0, remainingDistance=0;
    const uint8_t *current=nullptr, *next=nullptr;
    size_t currentSize=0, nextSize=0;
    const char *layout="";
};
inline void tail(const uint8_t *p, Nav &v) {
    v.hours=be16(p);v.minutes=p[2];v.timeThird=p[3];
    v.remainingDistance=be32(p+4);v.speed=be16(p+8);
}
inline bool variable(const uint8_t *p, size_t n, Nav &v, bool second=false) {
    if (n<(second?22u:17u)) return false;
    size_t distanceOffset=1;
    if (second) {
        const uint32_t image=be32(p+1);
        if (image==1) { if (p[5]>1) return false;distanceOffset=6; }
        else { if (image<2 || image>n-5) return false;distanceOffset=5+size_t(image); }
    }
    if (n-distanceOffset<16) return false;
    size_t o=distanceOffset+4;
    const size_t next=p[o++];
    if (next>n-o) return false;
    const uint8_t *nextData=p+o;o+=next;
    if (o>=n) return false;
    const size_t current=p[o++];
    if (current>n-o) return false;
    const uint8_t *currentData=p+o;o+=current;
    if (n-o!=10) return false;
    v.direction=p[0];v.distance=be32(p+distanceOffset);
    v.next=nextData;v.nextSize=next;v.current=currentData;v.currentSize=current;
    tail(p+o,v);v.layout=second?"variable2":"variable1";
    return true;
}
inline bool fixed(const uint8_t *p, size_t n, Nav &v) {
    if (n!=175) return false;
    v.direction=p[0];v.distance=be32(p+1);
    v.next=p+5;v.nextSize=80;v.current=p+85;v.currentSize=80;
    tail(p+165,v);v.layout="fixed";return true;
}
inline bool navigation(const Frame &frame, Nav &v) {
    if (frame.command==0x40) return variable(frame.payload,frame.size,v,true);
    if (frame.command!=0) return false;
    return frame.legacy?fixed(frame.payload,frame.size,v):variable(frame.payload,frame.size,v);
}
} // namespace vmhud
