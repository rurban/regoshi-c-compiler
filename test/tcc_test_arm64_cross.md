# TCC Test Suite Report for RCC

Generated: May 2026

## Summary

- **Total**: 154
- **Passed**: 64
- **Failed**: 90
- **Pass Rate**: 41%

## Detailed Results

| Test                        | Status       | Message                       |
| :-------------------------- | :----------- | :---------------------------- |
| 00_assignment               | EXEC_FAIL    | non-zero exit                 |
| 01_comment                  | PASS         | Output matches                |
| 02_printf                   | EXEC_FAIL    | non-zero exit                 |
| 03_struct                   | PASS         | Output matches                |
| 04_for                      | EXEC_FAIL    | non-zero exit                 |
| 05_array                    | EXEC_FAIL    | non-zero exit                 |
| 06_case                     | EXEC_FAIL    | non-zero exit                 |
| 07_function                 | EXEC_FAIL    | non-zero exit                 |
| 08_while                    | EXEC_FAIL    | non-zero exit                 |
| 09_do_while                 | EXEC_FAIL    | non-zero exit                 |
| 10_pointer                  | EXEC_FAIL    | non-zero exit                 |
| 11_precedence               | EXEC_FAIL    | non-zero exit                 |
| 12_hashdefine               | PASS         | Output matches                |
| 13_integer_literals         | EXEC_FAIL    | non-zero exit                 |
| 14_if                       | EXEC_FAIL    | non-zero exit                 |
| 15_recursion                | EXEC_FAIL    | non-zero exit                 |
| 16_nesting                  | EXEC_FAIL    | non-zero exit                 |
| 17_enum                     | EXEC_FAIL    | non-zero exit                 |
| 18_include                  | PASS         | Output matches                |
| 19_pointer_arithmetic       | EXEC_FAIL    | non-zero exit                 |
| 20_pointer_comparison       | EXEC_FAIL    | non-zero exit                 |
| 21_char_array               | EXEC_FAIL    | non-zero exit                 |
| 22_floating_point           | EXEC_FAIL    | non-zero exit                 |
| 23_type_coercion            | EXEC_FAIL    | non-zero exit                 |
| 24_math_library             | PASS         | Output matches                |
| 25_quicksort                | EXEC_FAIL    | non-zero exit                 |
| 26_character_constants      | PASS         | Output matches                |
| 27_sizeof                   | PASS         | Output matches                |
| 28_strings                  | PASS         | Output matches                |
| 29_array_address            | PASS         | Output matches                |
| 30_hanoi                    | EXEC_FAIL    | non-zero exit                 |
| 31_args                     | EXEC_FAIL    | non-zero exit                 |
| 32_led                      | EXEC_FAIL    | non-zero exit                 |
| 33_ternary_op               | COMPILE_FAIL | rcc returned non-zero         |
| 34_array_assignment         | PASS         | Output matches                |
| 35_sizeof                   | PASS         | Output matches                |
| 36_array_initialisers       | EXEC_FAIL    | non-zero exit                 |
| 37_sprintf                  | EXEC_FAIL    | non-zero exit                 |
| 38_multiple_array_index     | EXEC_FAIL    | non-zero exit                 |
| 39_typedef                  | EXEC_FAIL    | non-zero exit                 |
| 40_stdio                    | EXEC_FAIL    | non-zero exit                 |
| 41_hashif                   | PASS         | Output matches                |
| 42_function_pointer         | COMPILE_FAIL | rcc returned non-zero         |
| 43_void_param               | PASS         | Output matches                |
| 44_scoped_declarations      | EXEC_FAIL    | non-zero exit                 |
| 45_empty_for                | EXEC_FAIL    | non-zero exit                 |
| 46_grep                     | COMPILE_FAIL | rcc returned non-zero         |
| 47_switch_return            | EXEC_FAIL    | non-zero exit                 |
| 48_nested_break             | PASS         | Output matches                |
| 49_bracket_evaluation       | EXEC_FAIL    | non-zero exit                 |
| 50_logical_second_arg       | EXEC_FAIL    | non-zero exit                 |
| 51_static                   | PASS         | Output matches                |
| 52_unnamed_enum             | PASS         | Output matches                |
| 54_goto                     | EXEC_FAIL    | non-zero exit                 |
| 55_lshift_type              | EXEC_FAIL    | non-zero exit                 |
| 60_errors_and_warnings      | SKIP         | Skipped                       |
| 61_integers                 | PASS         | Output matches                |
| 64_macro_nesting            | MISMATCH     | Output does not match .expect |
| 67_macro_concat             | EXEC_FAIL    | non-zero exit                 |
| 70_floating_point_literals  | MISMATCH     | Output does not match .expect |
| 71_macro_empty_arg          | PASS         | Output matches                |
| 72_long_long_constant       | EXEC_FAIL    | non-zero exit                 |
| 73_arm64                    | EXEC_FAIL    | non-zero exit                 |
| 75_array_in_struct_init     | EXEC_FAIL    | non-zero exit                 |
| 76_dollars_in_identifiers   | EXEC_FAIL    | non-zero exit                 |
| 77_push_pop_macro           | PASS         | Output matches                |
| 78_vla_label                | EXEC_FAIL    | non-zero exit                 |
| 79_vla_continue             | EXEC_FAIL    | non-zero exit                 |
| 80_flexarray                | COMPILE_FAIL | rcc returned non-zero         |
| 81_types                    | PASS         | Output matches                |
| 82_attribs_position         | EXEC_FAIL    | non-zero exit                 |
| 83_utf8_in_identifiers      | PASS         | Output matches                |
| 84_hex-float                | PASS         | Output matches                |
| 85_asm-outside-function     | COMPILE_FAIL | rcc returned non-zero         |
| 86_memory-model             | PASS         | Output matches                |
| 87_dead_code                | EXEC_FAIL    | non-zero exit                 |
| 88_codeopt                  | EXEC_FAIL    | non-zero exit                 |
| 89_nocode_wanted            | EXEC_FAIL    | non-zero exit                 |
| 90_struct-init              | COMPILE_FAIL | rcc returned non-zero         |
| 91_ptr_longlong_arith32     | EXEC_FAIL    | non-zero exit                 |
| 92_enum_bitfield            | EXEC_FAIL    | non-zero exit                 |
| 93_integer_promotion        | EXEC_FAIL    | non-zero exit                 |
| 94_generic                  | EXEC_FAIL    | non-zero exit                 |
| 95_bitfields                | EXEC_FAIL    | non-zero exit                 |
| 95_bitfields_ms             | SKIP         | Skipped                       |
| 96_nodata_wanted            | SKIP         | Skipped                       |
| 97_utf8_string_literal      | EXEC_FAIL    | non-zero exit                 |
| 98_al_ax_extend             | SKIP         | Skipped                       |
| 99_fastcall                 | SKIP         | Skipped                       |
| 100_c99array-decls          | PASS         | Output matches                |
| 101_cleanup                 | COMPILE_FAIL | rcc returned non-zero         |
| 102_alignas                 | PASS         | Output matches                |
| 103_implicit_memmove        | PASS         | Output matches                |
| 104_inline                  | COMPILE_FAIL | rcc returned non-zero         |
| 105_local_extern            | PASS         | Output matches                |
| 106_versym                  | PASS         | Output matches                |
| 107_stack_safe              | MISMATCH     | Output does not match .expect |
| 108_constructor             | MISMATCH     | Output does not match .expect |
| 109_float_struct_calling    | MISMATCH     | Output does not match .expect |
| 110_average                 | EXEC_FAIL    | non-zero exit                 |
| 111_conversion              | EXEC_FAIL    | non-zero exit                 |
| 112_backtrace               | SKIP         | Skipped                       |
| 113_btdll                   | SKIP         | Skipped                       |
| 114_bound_signal            | SKIP         | Skipped                       |
| 115_bound_setjmp            | SKIP         | Skipped                       |
| 116_bound_setjmp2           | SKIP         | Skipped                       |
| 117_builtins                | EXEC_FAIL    | non-zero exit                 |
| 118_switch                  | MISMATCH     | Output does not match .expect |
| 119_random_stuff            | COMPILE_FAIL | rcc returned non-zero         |
| 120_alias                   | COMPILE_FAIL | rcc returned non-zero         |
| 121_struct_return           | EXEC_FAIL    | non-zero exit                 |
| 122_vla_reuse               | EXEC_FAIL    | non-zero exit                 |
| 123_vla_bug                 | EXEC_FAIL    | non-zero exit                 |
| 124_atomic_counter          | EXEC_FAIL    | non-zero exit                 |
| 125_atomic_misc             | MISMATCH     | Output differs                |
| 126_bound_global            | SKIP         | Skipped                       |
| 127_asm_goto                | SKIP         | Skipped                       |
| 128_run_atexit              | EXEC_FAIL    | non-zero exit                 |
| 129_scopes                  | EXEC_FAIL    | non-zero exit                 |
| 130_large_argument          | EXEC_FAIL    | non-zero exit                 |
| 131_return_struct_in_reg    | EXEC_FAIL    | non-zero exit                 |
| 132_bound_test              | EXEC_FAIL    | non-zero exit                 |
| 133_old_func                | MISMATCH     | Output does not match .expect |
| 134_double_to_signed        | MISMATCH     | Output does not match .expect |
| 135_func_arg_struct_compare | PASS         | Output matches                |
| 136_atomic_gcc_style        | EXEC_FAIL    | non-zero exit                 |
| 137_funcall_struct_args     | MISMATCH     | Output does not match .expect |
| 138_arm64_encoding          | EXEC_FAIL    | non-zero exit                 |
| 139_arm64_errors            | PASS         | Output matches                |
| 140_arm64_extasm            | EXEC_FAIL    | non-zero exit                 |
| 141_riscv_asm               | MISMATCH     | Output does not match .expect |
| 142_int_conversion          | COMPILE_FAIL | rcc returned non-zero         |
| test_arm64_asm              | PASS         | exit=132                      |
| test_atomic_op              | PASS         | exit=139                      |
| test_atomic_op2             | PASS         | exit=139                      |
| test_bitfields              | PASS         | exit=132                      |
| test_builtins               | PASS         | exit=139                      |
| test_elif2                  | PASS         | exit=0                        |
| test_elif_simple            | PASS         | exit=0                        |
| test_err                    | PASS         | compile error as expected     |
| test_fallthrough            | PASS         | exit=139                      |
| test_func                   | PASS         | exit=16                       |
| test_gperf                  | PASS         | exit=0                        |
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
| test_loop                   | PASS         | exit=124                      |
| test_macro                  | PASS         | exit=139                      |
| test_minimal                | PASS         | exit=0                        |
| test_nested_if              | PASS         | exit=0                        |
| test_ptr                    | PASS         | exit=139                      |
| test_real                   | PASS         | exit=0                        |
| test_self_include2          | PASS         | exit=1                        |
| test_signextend             | PASS         | exit=139                      |
| test_simple                 | PASS         | exit=1                        |
| test_simple2                | PASS         | exit=1                        |
| test_str                    | PASS         | exit=0                        |
| test_struct                 | PASS         | exit=0                        |
| test_with_comment           | PASS         | exit=0                        |
