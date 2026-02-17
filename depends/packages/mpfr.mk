package=mpfr
$(package)_version=4.2.1
$(package)_download_path=https://ftp.gnu.org/gnu/mpfr/
$(package)_file_name=$(package)-$($(package)_version).tar.xz
$(package)_sha256_hash=277807353a6726978996945af13e52829e3abd7a9a5b7fb2793894e18f1fcbb2
$(package)_dependencies=gmp

define $(package)_set_vars
$(package)_config_opts=--disable-shared --with-gmp=$(host_prefix) CFLAGS="-O2 -fPIE"
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
