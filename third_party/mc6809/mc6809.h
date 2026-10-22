/********************************************
*
* Copyright 2012 by Sean Conner.  All Rights Reserved.
*
* This library is free software; you can redistribute it and/or modify it
* under the terms of the GNU Lesser General Public License as published by
* the Free Software Foundation; either version 3 of the License, or (at your
* option) any later version.
*
* This library is distributed in the hope that it will be useful, but
* WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY
* or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU Lesser General Public
* License for more details.
*
* You should have received a copy of the GNU Lesser General Public License
* along with this library; if not, see <http://www.gnu.org/licenses/>.
*
* Comments, questions and criticisms can be sent to: sean@conman.org
*
*********************************************/

#ifndef MC6809_H
#define MC6809_H

#include <stdint.h>
#include <stdbool.h>

#ifndef __GNUC__
#  define __attribute__(x)
#endif

#if defined(__BYTE_ORDER__) && __BYTE_ORDER__ == __ORDER_BIG_ENDIAN__
#  define MSB 0
#  define LSB 1
#elif defined(__BYTE_ORDER__) || defined(_WIN32)
#  define MSB 1
#  define LSB 0
#else
#  error You need to define the byte order
#endif

#ifndef _WIN32
#  define _cdecl
#  define __stdcall
#  define __declspec(x)
#endif

/************************************************************************/

typedef uint8_t  uint8;
typedef uint16_t uint16;
typedef int32_t  int32;
typedef uint32_t uint32;
typedef int64_t  int64;
typedef uint64_t uint64;

/************************************************************************/

// pointer types that call back into C#
typedef uint8(_cdecl* pReadFunction)(uint16 address);
typedef void(_cdecl* pWriteFunction)(uint16 address, uint8 value);
typedef void(_cdecl* pFaultFunction)(uint16 fault);
typedef void(_cdecl* pPeriodicCallback)(void);

/************************************************************************/

typedef enum
{
    MC6809_RESULT_OK = 0,
    MC6809_FAULT_INTERNAL_ERROR,
    MC6809_FAULT_INSTRUCTION,
    MC6809_FAULT_ADDRESS_MODE,
    MC6809_FAULT_EXG,
    MC6809_FAULT_TFR,
    
    MC6809_FAULT_UNDEFINED_READ,
    MC6809_FAULT_UNDEFINED_WRITE,
} MC6809_RESULT;

typedef union
{
    uint8 b[2];
    uint16 w;
} WORD;

/************************************************************************/

// The registers come first: the SH-2 reaches a byte only 15 bytes past a
// base register, a word 30 and a long 60, so the recompiled code
// (tools/recomp) addresses them all from one register. Changed from upstream.
typedef struct
{
    WORD D;
    bool CC_Z;
    bool CC_N;
    bool CC_C;
    bool CC_V;
    uint8 DP;
    bool CC_H;
    WORD Index[4];
    WORD PC;
    bool CC_E;
    bool CC_F;
    bool CC_I;
    bool    NMI_Armed;
    bool    NMI;
    bool    FIRQ;
    bool    IRQ;
    bool    CWAI;
    bool    SYNC;

    uint32  Clock;      // wraps; compare with (int32)(a - b)
    uint32  Limit;      // recompiled code runs until Clock reaches this

    pPeriodicCallback PeriodicCallback;
    uint32 CallbackCycles;
    int32 NextCallback;

    MC6809_RESULT Fault;

    uint8* ReadPointer[256];
    uint8* WritePointer[256];
    pReadFunction ReadFunction[256];
    pWriteFunction WriteFunction[256];
} mc6809__t;

#define X       Index[0]
#define Y       Index[1]
#define U       Index[2]
#define S       Index[3]
#define A       D.b[MSB]
#define B       D.b[LSB]

#endif