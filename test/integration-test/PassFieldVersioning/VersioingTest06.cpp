//===--------------------------------------------------------------------------------*- C++ -*-===//
//                         _     _ _              _            _
//                        (_)   | | |            | |          | |
//               __ _ _ __ _  __| | |_ ___   ___ | |___    ___| | __ _ _ __   __ _
//              / _` | '__| |/ _` | __/ _ \ / _ \| / __|  / __| |/ _` | '_ \ / _` |
//             | (_| | |  | | (_| | || (_) | (_) | \__ \ | (__| | (_| | | | | (_| |
//              \__, |_|  |_|\__,_|\__\___/ \___/|_|___/  \___|_|\__,_|_| |_|\__, |
//               __/ |                                                        __/ |
//              |___/                                                        |___/
//
//  This file is distributed under the MIT License (MIT).
//  See LICENSE.txt for details.
//
//===------------------------------------------------------------------------------------------===//

// RUN: %gtclang% %file% -fno-codegen -freport-pass-field-versioning

#include "gridtools/clang_dsl.hpp"

using namespace gridtools::clang;

stencil Test {
  storage field, tmp;

  Do {
    vertical_region(k_start, k_end) {
      tmp = field(i + 1); // EXPECTED: PASS: PassFieldVersioning: Test: rename:%line% field_1:field_2
      field = tmp;        // EXPECTED: PASS: PassFieldVersioning: Test: rename:%line% tmp:tmp_1

      tmp = field(i + 1); // EXPECTED: PASS: PassFieldVersioning: Test: rename:%line% field:field_1
      field = tmp;
    }
  }
};

int main() {}
