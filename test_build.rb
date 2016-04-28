#!/usr/bin/env ruby

BUILD_DIR = 'mruby_build'
if __FILE__ == $PROGRAM_NAME
  require 'fileutils'
  unless File.exists?(BUILD_DIR)
    system "git clone --depth 1 https://github.com/mruby/mruby.git #{BUILD_DIR}"
  end
  system(%Q[cd #{BUILD_DIR}; MRUBY_CONFIG=#{File.expand_path __FILE__} ./minirake #{ARGV.join(' ')}])
  system "ln -s #{BUILD_DIR}/build/host/bin bin"
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
  # conf.gem "#{BUILD_DIR}/mrbgems/mruby-time"
  # conf.gem "#{BUILD_DIR}/mrbgems/mruby-hash-ext"
  # conf.gem "#{BUILD_DIR}/mrbgems/mruby-eval"
  # conf.gem "#{BUILD_DIR}/mrbgems/mruby-exit" # for exiting from within a script
  # conf.gem "#{BUILD_DIR}/mrbgems/mruby-string-ext"
  #
  # conf.gem :github => 'pbosetti/mruby-io',                  :branch => 'master'
  # conf.gem :github => 'pbosetti/mruby-dir',                 :branch => 'master'
  # conf.gem :github => 'pbosetti/mruby-tempfile',            :branch => 'master'
  # conf.gem :github => 'pbosetti/mruby-pcre-regexp',         :branch => 'master'
  # conf.gem :github => 'pbosetti/mruby-yaml',                :branch => 'master'
  # conf.gem :github => 'pbosetti/mruby-merb',                :branch => 'master'
  # conf.gem :github => 'pbosetti/mruby-complex',             :branch => 'master'
  # conf.gem :github => 'pbosetti/mruby-serialport',          :branch => 'master'
  # conf.gem :github => 'pbosetti/mruby-shell',               :branch => 'master'
  # conf.gem :github => 'pbosetti/mruby-sinatic',             :branch => 'master'
  # conf.gem :github => 'iij/mruby-pack',                     :branch => 'master'
  # conf.gem :github => 'iij/mruby-socket',                   :branch => 'master'
  # conf.gem :github => 'iij/mruby-errno',                    :branch => 'master'
  # conf.gem :github => 'iij/mruby-process',                  :branch => 'master'
  # conf.gem :github => 'iij/mruby-iijson',                   :branch => 'master'
  # conf.gem :github => 'ksss/mruby-signal',                  :branch => 'master'
  #
  # conf.gem File.dirname(__FILE__)
  # conf.gem :github => 'pbosetti/mruby-emb-require', :branch => "master"
  # if g = conf.gems.find {|e| e.name.match /mruby-require/} then
  #   g.build.cc.flags << "-DMRBGEMS_ROOT=\\\"#{MRBGEMS_ROOT}\\\""
  #   g.build.linker.flags << "-ldl"
  # end
end