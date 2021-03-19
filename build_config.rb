# frozen_string_literal: true

MRuby::Build.new do |conf|
  toolchain ENV.fetch('TOOLCHAIN', :clang)

  conf.enable_debug
  conf.enable_test

  conf.gem __dir__
end
