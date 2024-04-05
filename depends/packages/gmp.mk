package=gmp
$(package)_version=6.3.0
$(package)_download_path=https://gmplib.org/download/gmp/
$(package)_file_name=$(package)-$($(package)_version).tar.bz2
$(package)_sha256_hash=ac28211a7cfb609bae2e2c8d6058d66c8fe96434f740cf6fe2e47b000d1c20cb

define $(package)_set_vars
$(package)_config_opts=--disable-shared --enable-cxx --enable-fat CFLAGS="-O2 -fPIE" CXXFLAGS="-O2 -fPIE"
endef

define $(package)_config_cmds
  ./configure $($(package)_config_opts) --build=$(build) --host=$(host) --prefix=$(host_prefix)
endef

define $(package)_build_cmds
  $(MAKE)
endef

define $(package)_stage_cmds
  $(MAKE) DESTDIR=$($(package)_staging_dir) install
endef
