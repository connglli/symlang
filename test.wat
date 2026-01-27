(memory 16)
(global $__stack_pointer (mut i32) (i32.const 1048576))
(func $main (result f32)
  (local $pc i32)
  (local $__old_sp i32)
  (local $a f32)
  (local $b f32)
  (local $c f32)
  f32.const 1
  local.set $a
  f32.const 2
  local.set $b
  f32.const 0
  local.set $c
  i32.const 0
  local.set $pc
  (loop $__symir_dispatch_loop
    (block $entry
      local.get $pc
      br_table 0 0
    ) ;; ^entry
    local.get $a
    local.get $b
    f32.add
    local.set $c
    local.get $c
    return
  ) ;; dispatch loop
  f32.const 0.0
)

(export "symir_main" (func $main))
