##
# Mutex test

assert('Object.const_defined? :Mutex') do
  assert_true Object.const_defined?(:Mutex)
end

assert('Mutex#lock') do
  # todo
end

assert('Mutex#unlock') do
  # todo
end

assert('Mutex#locked?') do
  m = Mutex.new
  assert_false m.locked?
  m.lock
  assert_true m.locked?
end

assert('Mutex#try_lock') do
  m = Mutex.new
  assert_true  m.try_lock
  assert_false m.try_lock
end

assert('Mutex#synchronize returns true') do
  m = Mutex.new
  assert_true(m.synchronize { true })
end

assert('Mutex#synchronize returns Fixnum') do
  m = Mutex.new
  assert_equal(42, m.synchronize { 42 })
end
