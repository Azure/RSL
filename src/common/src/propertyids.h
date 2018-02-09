#pragma once

// !!!!!!!! For Search Only
// In order to break the messy dependency loop, we had to fork
// the propertyids.h file. That means, the constants defined
// in that file (such as PropLengthMask, PropLength_Short)
// cannot change. That might sound bad, but in practice, these
// constants would be compiled into code that distribute around
// different environments with different release schedules that
// could be months apart. Thus, we never could change those values anyway.

#include "basic_types.h"

namespace RSLibImpl
{

// property type flag
const UInt16 PropTypeMask                       = 0xc000;

// PropType_Atom is a leaf property which is an element of a list or a
// set. There may be nested properties within the leaf.
const UInt16 PropType_Atom                      = 0x0000;

// length type flag
const UInt16 PropLengthMask                     = 0x2000;

// A property with PropLength_Short has a 1-byte length field
const UInt16 PropLength_Short                   = 0x0000;
// A property with PropLength_Long has a 4-byte length field
const UInt16 PropLength_Long                    = 0x2000;

// mask for the remaining 13-bit namespace

const UInt16 PropValueMask                      = 0x1fff;

#define PROP_SHORTATOM(x_) ((x_) | PropType_Atom | PropLength_Short)

// Propries for Cosmos
const UInt16 Prop_Stream_BeginTag          = PROP_SHORTATOM(0x1200);
const UInt16 Prop_Stream_EndTag            = PROP_SHORTATOM(0x1201);

} // namespace RSLibImpl