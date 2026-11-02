#pragma once
#include <string>
#include <stdexcept>
inline void require(bool ok, const char* what) { if (!ok) throw std::runtime_error(what); }
template<class F> void rejects(F f) {
    bool rejected=false;
    try { f(); } catch (const std::exception&) { rejected=true; }
    require(rejected,"expected rejection");
}
inline std::string ptx_fixture(const std::string& body = "ld.global.u32 %r1, [%rd2]; st.global.u32 [%rd3], %r1;", const std::string& name="read") {
    return ".version 8.0\n.target sm_75\n.address_size 64\n.visible .entry "+name+R"PTX((.param .u64 input, .param .u64 output) {
.reg .b64 %rd<10>;
.reg .b32 %r<16>;
.reg .f32 %f<4>;
.reg .f64 %d<4>;
.reg .pred %p<4>;
ld.param.u64 %rd0, [input];
ld.param.u64 %rd1, [output];
mov.u32 %r0, %tid.x;
mul.wide.u32 %rd4, %r0, 16;
add.u64 %rd2, %rd0, %rd4;
add.u64 %rd3, %rd1, %rd4;
)PTX"+body+"\nret;\n}\n";
}
inline void replace_one(std::string& text,const std::string& from,const std::string& to) {
    const auto at=text.find(from); require(at!=std::string::npos,"fixture replace missing"); text.replace(at,from.size(),to);
}
