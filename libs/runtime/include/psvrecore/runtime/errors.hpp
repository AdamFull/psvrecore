#pragma once

// Error codes system library functions return to the guest (negative 32-bit values), by the names the console's
// libraries give them.

#include "core/foundation/core/foundation.h"

namespace psvrecore::runtime::err {

inline constexpr i32 code(u32 value) { return static_cast<i32>(value); }

inline constexpr i32 kOk = 0;

// Kernel.
inline constexpr i32 kError = code(0x80020001);
inline constexpr i32 kUnsupported = code(0x80020004);
inline constexpr i32 kInvalidArgument = code(0x80020005);
inline constexpr i32 kIllegalAddress = code(0x80020006);
inline constexpr i32 kIllegalAlignment = code(0x80020007);
inline constexpr i32 kIllegalSize = code(0x8002000B);
inline constexpr i32 kIllegalType = code(0x8002000C);
inline constexpr i32 kIllegalAttr = code(0x8002000E);
inline constexpr i32 kIllegalCount = code(0x8002000F);
inline constexpr i32 kIllegalMode = code(0x80020010);
inline constexpr i32 kNoFreePhysicalPage = code(0x80024302);
inline constexpr i32 kUidOverflow = code(0x8002450D);
inline constexpr i32 kIllegalMemBlockType = code(0x80024B01);
inline constexpr i32 kIllegalMemBlockSize = code(0x80024B05);
inline constexpr i32 kIllegalContext = code(0x80027101);
inline constexpr i32 kNoMemory = code(0x8002710B);
inline constexpr i32 kUnknownUid = code(0x80028001);
inline constexpr i32 kCanNotWait = code(0x80028004);
inline constexpr i32 kWaitTimeout = code(0x80028005);
inline constexpr i32 kWaitDelete = code(0x80028006);
inline constexpr i32 kWaitCancel = code(0x80028007);
inline constexpr i32 kThreadError = code(0x80028020);
inline constexpr i32 kUnknownThreadId = code(0x80028021);
inline constexpr i32 kIllegalThreadId = code(0x80028022);
inline constexpr i32 kIllegalPriority = code(0x80028023);
inline constexpr i32 kIllegalStackSize = code(0x80028024);
inline constexpr i32 kIllegalCpuAffinityMask = code(0x80028025);
inline constexpr i32 kDormant = code(0x80028027);
inline constexpr i32 kNotDormant = code(0x80028028);
inline constexpr i32 kUnknownCallbackId = code(0x800280A1);
inline constexpr i32 kUnknownEventFlagId = code(0x800280E1);
inline constexpr i32 kEventFlagMulti = code(0x800280E2);
inline constexpr i32 kEventFlagCond = code(0x800280E3);
inline constexpr i32 kAlreadySent = code(0x80028121);
inline constexpr i32 kUnknownSemaphoreId = code(0x80028101);
inline constexpr i32 kSemaphoreZero = code(0x80028102);
inline constexpr i32 kSemaphoreOverflow = code(0x80028103);
inline constexpr i32 kUnknownMutexId = code(0x80028141);
inline constexpr i32 kMutexRecursive = code(0x80028142);
inline constexpr i32 kMutexLockOverflow = code(0x80028143);
inline constexpr i32 kMutexUnlockUnderflow = code(0x80028144);
inline constexpr i32 kMutexFailedToOwn = code(0x80028145);
inline constexpr i32 kMutexNotOwned = code(0x80028146);
inline constexpr i32 kUnknownLwMutexId = code(0x80028181);
inline constexpr i32 kLwMutexRecursive = code(0x80028182);
inline constexpr i32 kLwMutexLockOverflow = code(0x80028183);
inline constexpr i32 kLwMutexUnlockUnderflow = code(0x80028184);
inline constexpr i32 kLwMutexFailedToOwn = code(0x80028185);
inline constexpr i32 kLwMutexNotOwned = code(0x80028186);
inline constexpr i32 kUnknownCondId = code(0x800281A1);
inline constexpr i32 kUnknownLwCondId = code(0x800281C1);

// errno-style errors (files).
inline constexpr i32 kPerm = code(0x80010001);
inline constexpr i32 kNoEntry = code(0x80010002);
inline constexpr i32 kIo = code(0x80010005);
inline constexpr i32 kBadFile = code(0x80010009);
inline constexpr i32 kAgain = code(0x8001000B);
inline constexpr i32 kNoMem = code(0x8001000C);
inline constexpr i32 kAccess = code(0x8001000D);
inline constexpr i32 kFault = code(0x8001000E);
inline constexpr i32 kBusy = code(0x80010010);
inline constexpr i32 kExists = code(0x80010011);
inline constexpr i32 kCrossDevice = code(0x80010012);
inline constexpr i32 kNotDirectory = code(0x80010014);
inline constexpr i32 kIsDirectory = code(0x80010015);
inline constexpr i32 kInvalid = code(0x80010016);
inline constexpr i32 kTooManyOpen = code(0x80010017);  // ENFILE
inline constexpr i32 kTooManyFiles = code(0x80010018);  // EMFILE
inline constexpr i32 kFileTooBig = code(0x8001001B);
inline constexpr i32 kNoSpace = code(0x8001001C);
inline constexpr i32 kIllegalSeek = code(0x8001001D);
inline constexpr i32 kReadOnlyFs = code(0x8001001E);
inline constexpr i32 kNotEmpty = code(0x8001005A);
inline constexpr i32 kNameTooLong = code(0x8001005B);
inline constexpr i32 kOpNotSupported = code(0x8001005F);
inline constexpr i32 kNotSupported = code(0x80010086);

}  // namespace psvrecore::runtime::err
