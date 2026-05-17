# TCC Test Suite Report for RCC

Generated: May 2026

## Summary

- **Total**: 152
- **Passed**: 105
- **Failed**: 47
- **Pass Rate**: 69%

## Detailed Results

| Test                        | Status       | Message                       |
| :-------------------------- | :----------- | :---------------------------- |
| 00_assignment               | PASS         | Output matches                |
| 01_comment                  | PASS         | Output matches                |
| 02_printf                   | PASS         | Output matches                |
| 03_struct                   | PASS         | Output matches                |
| 04_for                      | PASS         | Output matches                |
| 05_array                    | PASS         | Output matches                |
| 06_case                     | PASS         | Output matches                |
| 07_function                 | PASS         | Output matches                |
| 08_while                    | PASS         | Output matches                |
| 09_do_while                 | PASS         | Output matches                |
| 10_pointer                  | PASS         | Output matches                |
| 11_precedence               | PASS         | Output matches                |
| 12_hashdefine               | PASS         | Output matches                |
| 13_integer_literals         | PASS         | Output matches                |
| 14_if                       | PASS         | Output matches                |
| 15_recursion                | PASS         | Output matches                |
| 16_nesting                  | PASS         | Output matches                |
| 17_enum                     | MISMATCH     | Output does not match .expect |
| 18_include                  | PASS         | Output matches                |
| 19_pointer_arithmetic       | PASS         | Output matches                |
| 20_pointer_comparison       | PASS         | Output matches                |
| 21_char_array               | PASS         | Output matches                |
| 22_floating_point           | EXEC_FAIL    | non-zero exit                 |
| 23_type_coercion            | EXEC_FAIL    | non-zero exit                 |
| 24_math_library             | EXEC_FAIL    | non-zero exit                 |
| 25_quicksort                | PASS         | Output matches                |
| 26_character_constants      | PASS         | Output matches                |
| 27_sizeof                   | PASS         | Output matches                |
| 28_strings                  | PASS         | Output matches                |
| 29_array_address            | PASS         | Output matches                |
| 30_hanoi                    | PASS         | Output matches                |
| 31_args                     | PASS         | Output matches                |
| 32_led                      | EXEC_FAIL    | non-zero exit                 |
| 33_ternary_op               | EXEC_FAIL    | non-zero exit                 |
| 34_array_assignment         | EXEC_FAIL    | non-zero exit                 |
| 35_sizeof                   | PASS         | Output matches                |
| 36_array_initialisers       | PASS         | Output matches                |
| 37_sprintf                  | PASS         | Output matches                |
| 38_multiple_array_index     | PASS         | Output matches                |
| 39_typedef                  | PASS         | Output matches                |
| 40_stdio                    | PASS         | Output matches                |
| 41_hashif                   | PASS         | Output matches                |
| 42_function_pointer         | PASS         | Output matches                |
| 43_void_param               | PASS         | Output matches                |
| 44_scoped_declarations      | PASS         | Output matches                |
| 45_empty_for                | PASS         | Output matches                |
| 46_grep                     | PASS         | Output matches                |
| 47_switch_return            | PASS         | Output matches                |
| 48_nested_break             | PASS         | Output matches                |
| 49_bracket_evaluation       | MISMATCH     | Output does not match .expect |
| 50_logical_second_arg       | PASS         | Output matches                |
| 51_static                   | PASS         | Output matches                |
| 52_unnamed_enum             | PASS         | Output matches                |
| 54_goto                     | PASS         | Output matches                |
| 55_lshift_type              | EXEC_FAIL    | non-zero exit                 |
| 60_errors_and_warnings      | SKIP         | Skipped                       |
| 61_integers                 | PASS         | Output matches                |
| 64_macro_nesting            | PASS         | Output matches                |
| 67_macro_concat             | PASS         | Output matches                |
| 70_floating_point_literals  | MISMATCH     | Output does not match .expect |
| 71_macro_empty_arg          | PASS         | Output matches                |
| 72_long_long_constant       | PASS         | Output matches                |
| 73_arm64                    | SKIP         | Skipped                       |
| 75_array_in_struct_init     | EXEC_FAIL    | non-zero exit                 |
| 76_dollars_in_identifiers   | PASS         | Output matches                |
| 77_push_pop_macro           | PASS         | Output matches                |
| 78_vla_label                | EXEC_FAIL    | non-zero exit                 |
| 79_vla_continue             | EXEC_FAIL    | non-zero exit                 |
| 80_flexarray                | EXEC_FAIL    | non-zero exit                 |
| 81_types                    | PASS         | Output matches                |
| 82_attribs_position         | PASS         | Output matches                |
| 83_utf8_in_identifiers      | MISMATCH     | Output does not match .expect |
| 84_hex-float                | PASS         | Output matches                |
| 85_asm-outside-function     | COMPILE_FAIL | rcc returned non-zero         |
| 86_memory-model             | PASS         | Output matches                |
| 87_dead_code                | EXEC_FAIL    | non-zero exit                 |
| 88_codeopt                  | PASS         | Output matches                |
| 89_nocode_wanted            | PASS         | Output matches                |
| 90_struct-init              | EXEC_FAIL    | non-zero exit                 |
| 91_ptr_longlong_arith32     | PASS         | Output matches                |
| 92_enum_bitfield            | MISMATCH     | Output does not match .expect |
| 93_integer_promotion        | MISMATCH     | Output does not match .expect |
| 94_generic                  | PASS         | Output matches                |
| 95_bitfields                | EXEC_FAIL    | non-zero exit                 |
| 95_bitfields_ms             | EXEC_FAIL    | non-zero exit                 |
| 96_nodata_wanted            | SKIP         | Skipped                       |
| 97_utf8_string_literal      | MISMATCH     | Output does not match .expect |
| 98_al_ax_extend             | SKIP         | Skipped                       |
| 99_fastcall                 | SKIP         | Skipped                       |
| 100_c99array-decls          | PASS         | Output matches                |
| 101_cleanup                 | EXEC_FAIL    | non-zero exit                 |
| 102_alignas                 | PASS         | Output matches                |
| 103_implicit_memmove        | PASS         | Output matches                |
| 104_inline                  | COMPILE_FAIL | rcc returned non-zero         |
| 105_local_extern            | PASS         | Output matches                |
| 106_versym                  | PASS         | Output matches                |
| 107_stack_safe              | PASS         | Output matches                |
| 108_constructor             | MISMATCH     | Output does not match .expect |
| 109_float_struct_calling    | MISMATCH     | Output does not match .expect |
| 110_average                 | MISMATCH     | Output does not match .expect |
| 111_conversion              | MISMATCH     | Output does not match .expect |
| 112_backtrace               | SKIP         | Skipped                       |
| 113_btdll                   | SKIP         | Skipped                       |
| 114_bound_signal            | SKIP         | Skipped                       |
| 115_bound_setjmp            | SKIP         | Skipped                       |
| 116_bound_setjmp2           | SKIP         | Skipped                       |
| 117_builtins                | MISMATCH     | Output does not match .expect |
| 118_switch                  | MISMATCH     | Output does not match .expect |
| 119_random_stuff            | EXEC_FAIL    | non-zero exit                 |
| 120_alias                   | COMPILE_FAIL | rcc returned non-zero         |
| 121_struct_return           | MISMATCH     | Output does not match .expect |
| 122_vla_reuse               | EXEC_FAIL    | non-zero exit                 |
| 123_vla_bug                 | PASS         | Output matches                |
| 124_atomic_counter          | EXEC_FAIL    | non-zero exit                 |
| 125_atomic_misc             | MISMATCH     | Output differs                |
| 126_bound_global            | SKIP         | Skipped                       |
| 127_asm_goto                | EXEC_FAIL    | non-zero exit                 |
| 128_run_atexit              | MISMATCH     | Output does not match .expect |
| 129_scopes                  | EXEC_FAIL    | non-zero exit                 |
| 130_large_argument          | MISMATCH     | Output does not match .expect |
| 131_return_struct_in_reg    | EXEC_FAIL    | non-zero exit                 |
| 132_bound_test              | MISMATCH     | Output does not match .expect |
| 133_old_func                | MISMATCH     | Output does not match .expect |
| 134_double_to_signed        | EXEC_FAIL    | non-zero exit                 |
| 135_func_arg_struct_compare | PASS         | Output matches                |
| 136_atomic_gcc_style        | MISMATCH     | Output does not match .expect |
| 137_funcall_struct_args     | MISMATCH     | Output does not match .expect |
| 138_arm64_encoding          | SKIP         | Skipped                       |
| 138_narrow_return_promotion | PASS         | Output matches                |
| 139_arm64_errors            | SKIP         | Skipped                       |
| 139_narrow_type_conversion  | PASS         | Output matches                |
| 140_arm64_extasm            | SKIP         | Skipped                       |
| 140_int_sign_extension      | PASS         | Output matches                |
| 141_riscv_asm_pseudo        | SKIP         | Skipped                       |
| 142_riscv_asm_longlong      | SKIP         | Skipped                       |
| 143_riscv_asm_farith        | SKIP         | Skipped                       |
| test_arm64_asm              | SKIP         | Skipped                       |
| test_atomic_op              | PASS         | exit=0                        |
| test_atomic_op2             | PASS         | exit=0                        |
| test_bitfields              | PASS         | exit=0                        |
| test_builtins               | PASS         | exit=0                        |
| test_elif2                  | PASS         | exit=0                        |
| test_elif_simple            | PASS         | exit=0                        |
| test_err                    | PASS         | compile error as expected     |
| test_fallthrough            | PASS         | exit=0                        |
| test_func                   | PASS         | exit=0                        |
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
| test_loop                   | PASS         | exit=0                        |
| test_macro                  | PASS         | exit=0                        |
| test_minimal                | PASS         | exit=0                        |
| test_nested_if              | PASS         | exit=0                        |
| test_ptr                    | PASS         | exit=0                        |
| test_real                   | PASS         | exit=0                        |
| test_self_include2          | PASS         | exit=1                        |
| test_signextend             | PASS         | exit=0                        |
| test_simple                 | PASS         | exit=1                        |
| test_simple2                | PASS         | exit=1                        |
| test_str                    | PASS         | exit=0                        |
| test_struct                 | PASS         | exit=0                        |
| test_with_comment           | PASS         | exit=0                        |
