##
# Mutex test

assert('Object.const_defined? :Mutex') do
  Object.const_defined?(:Mutex)
end

assert('Mutex#lock') do
  #todo
  true
end

assert('Mutex#unlock') do
  #todo
  true
end

assert('Mutex#locked?') do
  m = Mutex.new
  assert_false m.locked?
  m.lock
  assert_true  m.locked?
end

assert('Mutex#try_lock') do
  m = Mutex.new
  assert_true  m.try_lock
  assert_false m.try_lock
end

assert('Mutex#synchronize returns true') do
  m = Mutex.new
  m.synchronize {true} == true
end

assert('Mutex#synchronize returns Fixnum') do
  m = Mutex.new
  m.synchronize {42} == 42
end

