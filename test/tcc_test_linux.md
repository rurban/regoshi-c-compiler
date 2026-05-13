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
| 00_assignment               | EXEC_FAIL    | non-zero exit                 |
| 01_comment                  | EXEC_FAIL    | non-zero exit                 |
| 02_printf                   | EXEC_FAIL    | non-zero exit                 |
| 03_struct                   | EXEC_FAIL    | non-zero exit                 |
| 04_for                      | EXEC_FAIL    | non-zero exit                 |
| 05_array                    | MISMATCH     | Output does not match .expect |
| 06_case                     | EXEC_FAIL    | non-zero exit                 |
| 07_function                 | EXEC_FAIL    | non-zero exit                 |
| 08_while                    | EXEC_FAIL    | non-zero exit                 |
| 09_do_while                 | MISMATCH     | Output does not match .expect |
| 10_pointer                  | EXEC_FAIL    | non-zero exit                 |
| 11_precedence               | EXEC_FAIL    | non-zero exit                 |
| 12_hashdefine               | MISMATCH     | Output does not match .expect |
| 13_integer_literals         | EXEC_FAIL    | non-zero exit                 |
| 14_if                       | EXEC_FAIL    | non-zero exit                 |
| 15_recursion                | EXEC_FAIL    | non-zero exit                 |
| 16_nesting                  | MISMATCH     | Output does not match .expect |
| 17_enum                     | EXEC_FAIL    | non-zero exit                 |
| 18_include                  | EXEC_FAIL    | non-zero exit                 |
| 19_pointer_arithmetic       | MISMATCH     | Output does not match .expect |
| 20_pointer_comparison       | EXEC_FAIL    | non-zero exit                 |
| 21_char_array               | EXEC_FAIL    | non-zero exit                 |
| 22_floating_point           | EXEC_FAIL    | non-zero exit                 |
| 23_type_coercion            | EXEC_FAIL    | non-zero exit                 |
| 24_math_library             | EXEC_FAIL    | non-zero exit                 |
| 25_quicksort                | MISMATCH     | Output does not match .expect |
| 26_character_constants      | EXEC_FAIL    | non-zero exit                 |
| 27_sizeof                   | EXEC_FAIL    | non-zero exit                 |
| 28_strings                  | EXEC_FAIL    | non-zero exit                 |
| 29_array_address            | EXEC_FAIL    | non-zero exit                 |
| 30_hanoi                    | EXEC_FAIL    | non-zero exit                 |
| 31_args                     | MISMATCH     | Output does not match .expect |
| 32_led                      | MISMATCH     | Output does not match .expect |
| 33_ternary_op               | EXEC_FAIL    | non-zero exit                 |
| 34_array_assignment         | MISMATCH     | Output does not match .expect |
| 35_sizeof                   | EXEC_FAIL    | non-zero exit                 |
| 36_array_initialisers       | MISMATCH     | Output does not match .expect |
| 37_sprintf                  | EXEC_FAIL    | non-zero exit                 |
| 38_multiple_array_index     | MISMATCH     | Output does not match .expect |
| 39_typedef                  | EXEC_FAIL    | non-zero exit                 |
| 40_stdio                    | EXEC_FAIL    | non-zero exit                 |
| 41_hashif                   | EXEC_FAIL    | non-zero exit                 |
| 42_function_pointer         | EXEC_FAIL    | non-zero exit                 |
| 43_void_param               | MISMATCH     | Output does not match .expect |
| 44_scoped_declarations      | MISMATCH     | Output does not match .expect |
| 45_empty_for                | MISMATCH     | Output does not match .expect |
| 46_grep                     | EXEC_FAIL    | non-zero exit                 |
| 47_switch_return            | EXEC_FAIL    | non-zero exit                 |
| 48_nested_break             | EXEC_FAIL    | non-zero exit                 |
| 49_bracket_evaluation       | EXEC_FAIL    | non-zero exit                 |
| 50_logical_second_arg       | EXEC_FAIL    | non-zero exit                 |
| 51_static                   | EXEC_FAIL    | non-zero exit                 |
| 52_unnamed_enum             | EXEC_FAIL    | non-zero exit                 |
| 54_goto                     | EXEC_FAIL    | non-zero exit                 |
| 55_lshift_type              | EXEC_FAIL    | non-zero exit                 |
| 60_errors_and_warnings      | SKIP         | Skipped                       |
| 61_integers                 | EXEC_FAIL    | non-zero exit                 |
| 64_macro_nesting            | EXEC_FAIL    | non-zero exit                 |
| 67_macro_concat             | EXEC_FAIL    | non-zero exit                 |
| 70_floating_point_literals  | EXEC_FAIL    | non-zero exit                 |
| 71_macro_empty_arg          | MISMATCH     | Output does not match .expect |
| 72_long_long_constant       | EXEC_FAIL    | non-zero exit                 |
| 73_arm64                    | SKIP         | Skipped                       |
| 75_array_in_struct_init     | EXEC_FAIL    | non-zero exit                 |
| 76_dollars_in_identifiers   | EXEC_FAIL    | non-zero exit                 |
| 77_push_pop_macro           | EXEC_FAIL    | non-zero exit                 |
| 78_vla_label                | EXEC_FAIL    | non-zero exit                 |
| 79_vla_continue             | EXEC_FAIL    | non-zero exit                 |
| 80_flexarray                | PASS         | Output matches                |
| 81_types                    | PASS         | Output matches                |
| 82_attribs_position         | EXEC_FAIL    | non-zero exit                 |
| 83_utf8_in_identifiers      | MISMATCH     | Output does not match .expect |
| 84_hex-float                | MISMATCH     | Output does not match .expect |
| 85_asm-outside-function     | COMPILE_FAIL | rcc returned non-zero         |
| 86_memory-model             | EXEC_FAIL    | non-zero exit                 |
| 87_dead_code                | EXEC_FAIL    | non-zero exit                 |
| 88_codeopt                  | EXEC_FAIL    | non-zero exit                 |
| 89_nocode_wanted            | EXEC_FAIL    | non-zero exit                 |
| 90_struct-init              | EXEC_FAIL    | non-zero exit                 |
| 91_ptr_longlong_arith32     | MISMATCH     | Output does not match .expect |
| 92_enum_bitfield            | PASS         | Output matches                |
| 93_integer_promotion        | EXEC_FAIL    | non-zero exit                 |
| 94_generic                  | EXEC_FAIL    | non-zero exit                 |
| 95_bitfields                | EXEC_FAIL    | non-zero exit                 |
| 95_bitfields_ms             | EXEC_FAIL    | non-zero exit                 |
| 96_nodata_wanted            | SKIP         | Skipped                       |
| 97_utf8_string_literal      | EXEC_FAIL    | non-zero exit                 |
| 98_al_ax_extend             | SKIP         | Skipped                       |
| 99_fastcall                 | SKIP         | Skipped                       |
| 100_c99array-decls          | PASS         | Output matches                |
| 101_cleanup                 | EXEC_FAIL    | non-zero exit                 |
| 102_alignas                 | MISMATCH     | Output does not match .expect |
| 103_implicit_memmove        | PASS         | Output matches                |
| 104_inline                  | EXEC_FAIL    | non-zero exit                 |
| 105_local_extern            | MISMATCH     | Output does not match .expect |
| 106_versym                  | MISMATCH     | Output does not match .expect |
| 107_stack_safe              | MISMATCH     | Output does not match .expect |
| 108_constructor             | MISMATCH     | Output does not match .expect |
| 109_float_struct_calling    | EXEC_FAIL    | non-zero exit                 |
| 110_average                 | MISMATCH     | Output does not match .expect |
| 111_conversion              | EXEC_FAIL    | non-zero exit                 |
| 112_backtrace               | SKIP         | Skipped                       |
| 113_btdll                   | SKIP         | Skipped                       |
| 114_bound_signal            | SKIP         | Skipped                       |
| 115_bound_setjmp            | SKIP         | Skipped                       |
| 116_bound_setjmp2           | SKIP         | Skipped                       |
| 117_builtins                | COMPILE_FAIL | rcc returned non-zero         |
| 118_switch                  | EXEC_FAIL    | non-zero exit                 |
| 119_random_stuff            | EXEC_FAIL    | non-zero exit                 |
| 120_alias                   | COMPILE_FAIL | rcc returned non-zero         |
| 121_struct_return           | MISMATCH     | Output does not match .expect |
| 122_vla_reuse               | MISMATCH     | Output does not match .expect |
| 123_vla_bug                 | EXEC_FAIL    | non-zero exit                 |
| 124_atomic_counter          | EXEC_FAIL    | non-zero exit                 |
| 125_atomic_misc             | MISMATCH     | Output differs                |
| 126_bound_global            | SKIP         | Skipped                       |
| 127_asm_goto                | MISMATCH     | Output does not match .expect |
| 128_run_atexit              | EXEC_FAIL    | non-zero exit                 |
| 129_scopes                  | EXEC_FAIL    | non-zero exit                 |
| 130_large_argument          | EXEC_FAIL    | non-zero exit                 |
| 131_return_struct_in_reg    | EXEC_FAIL    | non-zero exit                 |
| 132_bound_test              | EXEC_FAIL    | non-zero exit                 |
| 133_old_func                | EXEC_FAIL    | non-zero exit                 |
| 134_double_to_signed        | EXEC_FAIL    | non-zero exit                 |
| 135_func_arg_struct_compare | MISMATCH     | Output does not match .expect |
| 136_atomic_gcc_style        | EXEC_FAIL    | non-zero exit                 |
| 137_funcall_struct_args     | MISMATCH     | Output does not match .expect |
| 138_arm64_encoding          | SKIP         | Skipped                       |
| 138_narrow_return_promotion | EXEC_FAIL    | non-zero exit                 |
| 139_arm64_errors            | SKIP         | Skipped                       |
| 139_narrow_type_conversion  | EXEC_FAIL    | non-zero exit                 |
| 140_arm64_extasm            | SKIP         | Skipped                       |
| 140_int_sign_extension      | EXEC_FAIL    | non-zero exit                 |
| 141_riscv_asm_pseudo        | SKIP         | Skipped                       |
| 142_riscv_asm_longlong      | SKIP         | Skipped                       |
| 143_riscv_asm_farith        | SKIP         | Skipped                       |
| test_arm64_asm              | SKIP         | Skipped                       |
| test_atomic_op              | PASS         | exit=139                      |
| test_atomic_op2             | PASS         | exit=139                      |
| test_bitfields              | PASS         | exit=139                      |
| test_builtins               | PASS         | exit=139                      |
| test_elif2                  | PASS         | exit=76                       |
| test_elif_simple            | PASS         | exit=76                       |
| test_err                    | PASS         | compile error as expected     |
| test_fallthrough            | PASS         | exit=0                        |
| test_func                   | PASS         | exit=0                        |
| test_gperf                  | PASS         | exit=0                        |
| test_if                     | PASS         | exit=0                        |
| test_if2                    | PASS         | exit=0                        |
| test_if3                    | PASS         | exit=0                        |
| test_if4                    | PASS         | exit=0                        |
| test_if5                    | PASS         | exit=76                       |
| test_if6                    | PASS         | exit=76                       |
| test_if_nested              | PASS         | exit=76                       |
| test_if_simple              | PASS         | exit=76                       |
| test_include                | PASS         | exit=76                       |
| test_include2               | PASS         | exit=76                       |
| test_loop                   | PASS         | exit=1                        |
| test_macro                  | PASS         | exit=1                        |
| test_minimal                | PASS         | exit=0                        |
| test_nested_if              | PASS         | exit=0                        |
| test_ptr                    | PASS         | exit=20                       |
| test_real                   | PASS         | exit=0                        |
| test_self_include2          | PASS         | exit=76                       |
| test_signextend             | PASS         | exit=139                      |
| test_simple                 | PASS         | exit=76                       |
| test_simple2                | PASS         | exit=76                       |
| test_str                    | PASS         | exit=0                        |
| test_struct                 | PASS         | exit=0                        |
| test_with_comment           | PASS         | exit=0                        |
