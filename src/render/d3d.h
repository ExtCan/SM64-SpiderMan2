// Single place that includes the D3D12/DXGI headers.
//
// MinGW-w64's widl-generated d3d12.h declares COM methods that return structs
// by value (GetDesc, GetCPUDescriptorHandleForHeapStart, ...) with GCC's
// return convention unless WIDL_EXPLICIT_AGGREGATE_RETURNS is defined, which
// does not match the real runtime (MSVC ABI: hidden return pointer). Defining
// it makes MinGW builds call those methods correctly. MSVC ignores it.
#pragma once

#ifdef _WIN32

#ifndef WIDL_EXPLICIT_AGGREGATE_RETURNS
#define WIDL_EXPLICIT_AGGREGATE_RETURNS
#endif

#include "../common/platform.h"

#include <d3d12.h>
#include <d3dcompiler.h>
#include <dxgi1_6.h>

#endif
