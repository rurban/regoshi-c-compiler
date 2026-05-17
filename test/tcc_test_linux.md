# TCC Test Suite Report for RCC

Generated: May 2026

## Summary

- **Total**: 152
- **Passed**: 38
- **Failed**: 114
- **Pass Rate**: 25%

## Detailed Results

| Test                        | Status       | Message                       |
| :-------------------------- | :----------- | :---------------------------- |
| 00_assignment               | COMPILE_FAIL | rcc returned non-zero         |
| 01_comment                  | COMPILE_FAIL | rcc returned non-zero         |
| 02_printf                   | COMPILE_FAIL | rcc returned non-zero         |
| 03_struct                   | PASS         | Output matches                |
| 04_for                      | COMPILE_FAIL | rcc returned non-zero         |
| 05_array                    | COMPILE_FAIL | rcc returned non-zero         |
| 06_case                     | COMPILE_FAIL | rcc returned non-zero         |
| 07_function                 | COMPILE_FAIL | rcc returned non-zero         |
| 08_while                    | COMPILE_FAIL | rcc returned non-zero         |
| 09_do_while                 | COMPILE_FAIL | rcc returned non-zero         |
| 10_pointer                  | COMPILE_FAIL | rcc returned non-zero         |
| 11_precedence               | PASS         | Output matches                |
| 12_hashdefine               | COMPILE_FAIL | rcc returned non-zero         |
| 13_integer_literals         | COMPILE_FAIL | rcc returned non-zero         |
| 14_if                       | COMPILE_FAIL | rcc returned non-zero         |
| 15_recursion                | COMPILE_FAIL | rcc returned non-zero         |
| 16_nesting                  | COMPILE_FAIL | rcc returned non-zero         |
| 17_enum                     | COMPILE_FAIL | rcc returned non-zero         |
| 18_include                  | COMPILE_FAIL | rcc returned non-zero         |
| 19_pointer_arithmetic       | COMPILE_FAIL | rcc returned non-zero         |
| 20_pointer_comparison       | COMPILE_FAIL | rcc returned non-zero         |
| 21_char_array               | COMPILE_FAIL | rcc returned non-zero         |
| 22_floating_point           | COMPILE_FAIL | rcc returned non-zero         |
| 23_type_coercion            | COMPILE_FAIL | rcc returned non-zero         |
| 24_math_library             | COMPILE_FAIL | rcc returned non-zero         |
| 25_quicksort                | COMPILE_FAIL | rcc returned non-zero         |
| 26_character_constants      | COMPILE_FAIL | rcc returned non-zero         |
| 27_sizeof                   | COMPILE_FAIL | rcc returned non-zero         |
| 28_strings                  | COMPILE_FAIL | rcc returned non-zero         |
| 29_array_address            | COMPILE_FAIL | rcc returned non-zero         |
| 30_hanoi                    | COMPILE_FAIL | rcc returned non-zero         |
| 31_args                     | COMPILE_FAIL | rcc returned non-zero         |
| 32_led                      | COMPILE_FAIL | rcc returned non-zero         |
| 33_ternary_op               | COMPILE_FAIL | rcc returned non-zero         |
| 34_array_assignment         | COMPILE_FAIL | rcc returned non-zero         |
| 35_sizeof                   | COMPILE_FAIL | rcc returned non-zero         |
| 36_array_initialisers       | COMPILE_FAIL | rcc returned non-zero         |
| 37_sprintf                  | COMPILE_FAIL | rcc returned non-zero         |
| 38_multiple_array_index     | COMPILE_FAIL | rcc returned non-zero         |
| 39_typedef                  | COMPILE_FAIL | rcc returned non-zero         |
| 40_stdio                    | COMPILE_FAIL | rcc returned non-zero         |
| 41_hashif                   | COMPILE_FAIL | rcc returned non-zero         |
| 42_function_pointer         | COMPILE_FAIL | rcc returned non-zero         |
| 43_void_param               | COMPILE_FAIL | rcc returned non-zero         |
| 44_scoped_declarations      | COMPILE_FAIL | rcc returned non-zero         |
| 45_empty_for                | COMPILE_FAIL | rcc returned non-zero         |
| 46_grep                     | COMPILE_FAIL | rcc returned non-zero         |
| 47_switch_return            | COMPILE_FAIL | rcc returned non-zero         |
| 48_nested_break             | COMPILE_FAIL | rcc returned non-zero         |
| 49_bracket_evaluation       | COMPILE_FAIL | rcc returned non-zero         |
| 50_logical_second_arg       | COMPILE_FAIL | rcc returned non-zero         |
| 51_static                   | COMPILE_FAIL | rcc returned non-zero         |
| 52_unnamed_enum             | COMPILE_FAIL | rcc returned non-zero         |
| 54_goto                     | COMPILE_FAIL | rcc returned non-zero         |
| 55_lshift_type              | COMPILE_FAIL | rcc returned non-zero         |
| 60_errors_and_warnings      | SKIP         | Skipped                       |
| 61_integers                 | COMPILE_FAIL | rcc returned non-zero         |
| 64_macro_nesting            | COMPILE_FAIL | rcc returned non-zero         |
| 67_macro_concat             | COMPILE_FAIL | rcc returned non-zero         |
| 70_floating_point_literals  | COMPILE_FAIL | rcc returned non-zero         |
| 71_macro_empty_arg          | COMPILE_FAIL | rcc returned non-zero         |
| 72_long_long_constant       | COMPILE_FAIL | rcc returned non-zero         |
| 73_arm64                    | SKIP         | Skipped                       |
| 75_array_in_struct_init     | COMPILE_FAIL | rcc returned non-zero         |
| 76_dollars_in_identifiers   | COMPILE_FAIL | rcc returned non-zero         |
| 77_push_pop_macro           | COMPILE_FAIL | rcc returned non-zero         |
| 78_vla_label                | COMPILE_FAIL | rcc returned non-zero         |
| 79_vla_continue             | COMPILE_FAIL | rcc returned non-zero         |
| 80_flexarray                | COMPILE_FAIL | rcc returned non-zero         |
| 81_types                    | PASS         | Output matches                |
| 82_attribs_position         | PASS         | Output matches                |
| 83_utf8_in_identifiers      | COMPILE_FAIL | rcc returned non-zero         |
| 84_hex-float                | PASS         | Output matches                |
| 85_asm-outside-function     | COMPILE_FAIL | rcc returned non-zero         |
| 86_memory-model             | COMPILE_FAIL | rcc returned non-zero         |
| 87_dead_code                | EXEC_FAIL    | non-zero exit                 |
| 88_codeopt                  | COMPILE_FAIL | rcc returned non-zero         |
| 89_nocode_wanted            | PASS         | Output matches                |
| 90_struct-init              | COMPILE_FAIL | rcc returned non-zero         |
| 91_ptr_longlong_arith32     | PASS         | Output matches                |
| 92_enum_bitfield            | MISMATCH     | Output does not match .expect |
| 93_integer_promotion        | MISMATCH     | Output does not match .expect |
| 94_generic                  | COMPILE_FAIL | rcc returned non-zero         |
| 95_bitfields                | COMPILE_FAIL | rcc returned non-zero         |
| 95_bitfields_ms             | COMPILE_FAIL | rcc returned non-zero         |
| 96_nodata_wanted            | SKIP         | Skipped                       |
| 97_utf8_string_literal      | COMPILE_FAIL | rcc returned non-zero         |
| 98_al_ax_extend             | SKIP         | Skipped                       |
| 99_fastcall                 | SKIP         | Skipped                       |
| 100_c99array-decls          | PASS         | Output matches                |
| 101_cleanup                 | EXEC_FAIL    | non-zero exit                 |
| 102_alignas                 | PASS         | Output matches                |
| 103_implicit_memmove        | MISMATCH     | Output does not match .expect |
| 104_inline                  | COMPILE_FAIL | rcc returned non-zero         |
| 105_local_extern            | PASS         | Output matches                |
| 106_versym                  | COMPILE_FAIL | rcc returned non-zero         |
| 107_stack_safe              | PASS         | Output matches                |
| 108_constructor             | MISMATCH     | Output does not match .expect |
| 109_float_struct_calling    | COMPILE_FAIL | rcc returned non-zero         |
| 110_average                 | COMPILE_FAIL | rcc returned non-zero         |
| 111_conversion              | COMPILE_FAIL | rcc returned non-zero         |
| 112_backtrace               | SKIP         | Skipped                       |
| 113_btdll                   | SKIP         | Skipped                       |
| 114_bound_signal            | SKIP         | Skipped                       |
| 115_bound_setjmp            | SKIP         | Skipped                       |
| 116_bound_setjmp2           | SKIP         | Skipped                       |
| 117_builtins                | COMPILE_FAIL | rcc returned non-zero         |
| 118_switch                  | COMPILE_FAIL | rcc returned non-zero         |
| 119_random_stuff            | COMPILE_FAIL | rcc returned non-zero         |
| 120_alias                   | COMPILE_FAIL | rcc returned non-zero         |
| 121_struct_return           | COMPILE_FAIL | rcc returned non-zero         |
| 122_vla_reuse               | COMPILE_FAIL | rcc returned non-zero         |
| 123_vla_bug                 | COMPILE_FAIL | rcc returned non-zero         |
| 124_atomic_counter          | COMPILE_FAIL | rcc returned non-zero         |
| 125_atomic_misc             | MISMATCH     | Output differs                |
| 126_bound_global            | SKIP         | Skipped                       |
| 127_asm_goto                | EXEC_FAIL    | non-zero exit                 |
| 128_run_atexit              | COMPILE_FAIL | executable missing            |
| 129_scopes                  | EXEC_FAIL    | non-zero exit                 |
| 130_large_argument          | COMPILE_FAIL | rcc returned non-zero         |
| 131_return_struct_in_reg    | COMPILE_FAIL | rcc returned non-zero         |
| 132_bound_test              | COMPILE_FAIL | rcc returned non-zero         |
| 133_old_func                | MISMATCH     | Output does not match .expect |
| 134_double_to_signed        | COMPILE_FAIL | rcc returned non-zero         |
| 135_func_arg_struct_compare | COMPILE_FAIL | rcc returned non-zero         |
| 136_atomic_gcc_style        | COMPILE_FAIL | rcc returned non-zero         |
| 137_funcall_struct_args     | COMPILE_FAIL | rcc returned non-zero         |
| 138_arm64_encoding          | SKIP         | Skipped                       |
| 138_narrow_return_promotion | COMPILE_FAIL | rcc returned non-zero         |
| 139_arm64_errors            | SKIP         | Skipped                       |
| 139_narrow_type_conversion  | COMPILE_FAIL | rcc returned non-zero         |
| 140_arm64_extasm            | SKIP         | Skipped                       |
| 140_int_sign_extension      | COMPILE_FAIL | rcc returned non-zero         |
| 141_riscv_asm_pseudo        | SKIP         | Skipped                       |
| 142_riscv_asm_longlong      | SKIP         | Skipped                       |
| 143_riscv_asm_farith        | SKIP         | Skipped                       |
| test_arm64_asm              | SKIP         | Skipped                       |
| test_atomic_op              | COMPILE_FAIL | rcc returned non-zero         |
| test_atomic_op2             | COMPILE_FAIL | rcc returned non-zero         |
| test_bitfields              | PASS         | exit=0                        |
| test_builtins               | COMPILE_FAIL | rcc returned non-zero         |
| test_elif2                  | PASS         | exit=0                        |
| test_elif_simple            | PASS         | exit=0                        |
| test_err                    | PASS         | compile error as expected     |
| test_fallthrough            | PASS         | exit=0                        |
| test_func                   | PASS         | exit=0                        |
| test_gperf                  | COMPILE_FAIL | rcc returned non-zero         |
| test_if                     | PASS         | exit=0                        |
| test_if2                    | PASS         | exit=0                        |
| test_if3                    | PASS         | exit=0                        |
| test_if4                    | PASS         | exit=0                        |
| test_if5                    | PASS         | exit=0                        |
| test_if6                    | PASS         | exit=0                        |
| test_if_nested              | PASS         | exit=0                        |
| test_if_simple              | PASS         | exit=0                        |
| test_include                | PASS         | exit=42                       |
| test_include2               | PASS         | exit=10                       |
| test_loop                   | PASS         | exit=0                        |
| test_macro                  | PASS         | exit=0                        |
| test_minimal                | PASS         | exit=0                        |
| test_nested_if              | PASS         | exit=0                        |
| test_ptr                    | PASS         | exit=0                        |
| test_real                   | COMPILE_FAIL | rcc returned non-zero         |
| test_self_include2          | PASS         | exit=1                        |
| test_signextend             | COMPILE_FAIL | rcc returned non-zero         |
| test_simple                 | PASS         | exit=1                        |
| test_simple2                | PASS         | exit=1                        |
| test_str                    | PASS         | exit=0                        |
| test_struct                 | PASS         | exit=0                        |
| test_with_comment           | PASS         | exit=0                        |
