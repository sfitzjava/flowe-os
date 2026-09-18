#pragma once
#include <cstdint>
namespace BoardConfig {
constexpr uint32_t MAX_FRAMEBUFFER_BYTES = 52272;
enum class Board {XteinkX3, XteinkX3Uc8279};
struct Pins {int8_t sclk=0,mosi=0,cs=0,dc=0,rst=0,busy=0,powerEnable=0;};
struct Profile {Board board=Board::XteinkX3; Pins display;};
inline Profile ACTIVE;
inline void selectDevice(Board b){ACTIVE.board=b;}
}
