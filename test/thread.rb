##
# Thread test

assert('Object.const_defined? :Thread') do
 Object.const_defined?(:Thread)
end

assert('Thread returns Fixnum') do
  a = Thread.new{100}
  a.join == 100
end

assert('Thread returns true') do
  a = Thread.new{true}
  a.join == true
end

assert('Thread returns false') do
  a = Thread.new{false}
  a.join == false
end

assert('Thread returns Float') do
  a = Thread.new{99.99}
  a.join == 99.99
end

assert('Thread returns String') do
  a = Thread.new{"hello"}
  a.join == "hello"
end

assert('Thread returns Symbol') do
  a = Thread.new{:context}
  assert_true a.join == :context
end

assert('Thread returns Array') do
  a = Thread.new{[1,2,3]}
  a.join == [1,2,3]
end

assert('Thread returns Hash') do
  a = Thread.new{{'abc_key' => 'abc_value', 'cba_key' => 'cba_value'}}
  a.join == {'abc_key' => 'abc_value', 'cba_key' => 'cba_value'}
end

assert('Thread migrates Fixnum') do
  a = Thread.new(100){|a| a}
  a.join == 100
end

assert('Thread migrates ture') do
  a = Thread.new(true){|a| a}
  a.join == true
end

assert('Thread migrates false') do
  a = Thread.new(false){|a| a}
  a.join == false
end

assert('Thread migrates Float') do
  a = Thread.new(99.99){|a| a}
  a.join == 99.99
end

assert('Thread migrates String') do
  a = Thread.new("hello"){|a| a}
  a.join == "hello"
end

assert('Thread migrates Symbol') do
  a = Thread.new(:context){|a| a}
  assert_true a.join == :context
end

assert('Thread migrates Symbol in a complex context') do
  a = :cxt1
  t = Thread.new(:cxt2){|b| [a, b, :ctx3] }
  assert_equal [:cxt1, :cxt2, :ctx3], t.join
end

assert('Thread migrates Array') do
  skip "skip because COPY_VALUES is disabled" unless Thread::COPY_VALUES
  a = Thread.new([1,2,3]){|a| a}
  a.join == [1,2,3]
end

assert('Thread migrates Hash') do
  skip "skip because COPY_VALUES is disabled" unless Thread::COPY_VALUES
  a = Thread.new({'abc_key' => 'abc_value', 'cba_key' => 'cba_value'}){|a| a}
  a.join == {'abc_key' => 'abc_value', 'cba_key' => 'cba_value'}
end

assert('Thread migrates Proc') do
  pr = Proc.new { 1 }
  a = Thread.new(pr){|pr| pr.call }
  a.join == 1
end

class DummyObj
  attr_accessor :foo, :bar, :buz
end

assert('Thread migrates Object') do
  skip

  a = DummyObj.new
  t = Thread.new(a) do |a|
    a.foo = "foo"
    a.bar = 123
    a.buz = :buz
    a
  end
  a = t.join

  assert_equal DummyObj, a.class
  assert_equal "foo",    a.foo
  assert_equal 123,      a.bar
  assert_equal :buz,     a.buz
end

assert('Fixed test of issue #36') do
  a = Thread.new do
    "".is_a?(String)
  end
  assert_true a.join
end

assert('Thread GC') do
  t = Thread.new { GC.start }
  t.join
end
