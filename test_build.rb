#!/usr/bin/env ruby

BUILD_DIR = 'mruby_build'
if __FILE__ == $PROGRAM_NAME
  require 'fileutils'
  unless File.exists?(BUILD_DIR)
    system "git clone --depth 1 https://github.com/mruby/mruby.git #{BUILD_DIR}"
  end
  system(%Q[cd #{BUILD_DIR}; MRUBY_CONFIG=#{File.expand_path __FILE__} ./minirake #{ARGV.join(' ')}])
  system "ln -fs #{BUILD_DIR}/build/host/bin bin"
  exit
end

MRuby::Build.new do |conf|
  MRBGEMS_ROOT = "/usr/local/mrblib"
  toolchain :clang
  conf.cc.defines += %w(ENABLE_READLINE)
  conf.cc.include_paths << %w(/usr/local/include)
  conf.linker.library_paths << %w(/usr/local/lib)
  conf.linker.libraries << ['pthread']
  conf.gembox 'default'
  conf.gem File.dirname(__FILE__)
end
