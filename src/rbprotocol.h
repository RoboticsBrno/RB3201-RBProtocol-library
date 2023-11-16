#pragma once

#include "rbprotocoludp.h"

#define RBPROTOCOL_AXIS_MIN (-32767) //!< Minimal value of axes in "joy" command
#define RBPROTOCOL_AXIS_MAX (32767) //!< Maximal value of axes in "joy" command


// Backwards compatibility
namespace rb {
using Protocol = ProtocolUdp;
};
