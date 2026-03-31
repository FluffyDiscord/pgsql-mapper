#!/bin/bash
set -e

cd "$(dirname "$0")"

# ── Package hints per distro ──────────────────────────────────────────────────
hint() {
    local pkg="$1"
    if   command -v zypper &>/dev/null; then echo "  zypper install $pkg"
    elif command -v apt-get &>/dev/null; then echo "  apt-get install $pkg"
    elif command -v dnf     &>/dev/null; then echo "  dnf install $pkg"
    elif command -v yum     &>/dev/null; then echo "  yum install $pkg"
    fi
}

MISSING=0

# ── Required binaries ─────────────────────────────────────────────────────────
for bin in gcc make phpize php-config pkg-config; do
    if ! command -v "$bin" &>/dev/null; then
        echo "ERROR: '$bin' not found."
        case "$bin" in
            gcc)        hint "gcc" ;;
            make)       hint "make" ;;
            phpize)     hint "php8-devel   # or php-devel" ;;
            php-config) hint "php8-devel   # or php-devel" ;;
            pkg-config) hint "pkg-config" ;;
        esac
        MISSING=1
    fi
done

# ── libpq headers (postgresql-devel) ─────────────────────────────────────────
LIBPQ_H=""
if command -v pkg-config &>/dev/null && pkg-config --exists libpq 2>/dev/null; then
    LIBPQ_H=$(pkg-config --variable=includedir libpq)/libpq-fe.h
elif command -v pg_config &>/dev/null; then
    LIBPQ_H="$(pg_config --includedir)/libpq-fe.h"
else
    # Fallback: common paths
    for p in /usr/include/libpq-fe.h /usr/include/pgsql/libpq-fe.h /usr/include/postgresql/libpq-fe.h; do
        if [ -f "$p" ]; then LIBPQ_H="$p"; break; fi
    done
fi

if [ -z "$LIBPQ_H" ] || [ ! -f "$LIBPQ_H" ]; then
    echo "ERROR: libpq-fe.h not found (PostgreSQL client development headers missing)."
    hint "libpq-devel   # or postgresql-devel"
    MISSING=1
fi

# ── PHP pgsql extension must be available (runtime dep) ───────────────────────
if ! php -r 'exit(extension_loaded("pgsql") ? 0 : 1);' 2>/dev/null; then
    echo "ERROR: PHP 'pgsql' extension not loaded (required at runtime)."
    hint "php8-pgsql   # or php-pgsql"
    MISSING=1
fi

if [ "$MISSING" -ne 0 ]; then
    echo ""
    echo "Install the missing dependencies above and re-run this script."
    exit 1
fi

# ── If configure hasn't been run yet, run phpize + configure ──────────────────
if [ ! -f Makefile ]; then
    echo "No Makefile found — running phpize && ./configure ..."
    phpize
    ./configure --enable-pgsql_mapper
fi

# ── Build & install ───────────────────────────────────────────────────────────
make clean 2>/dev/null || true
make

EXTENSION_DIR=$(php-config --extension-dir)
INI_DIR=$(php --ini | grep "Scan for additional" | sed 's/.*: //')

sudo cp modules/pgsql_mapper.so "$EXTENSION_DIR/"
if [ -n "$INI_DIR" ] && [ -d "$INI_DIR" ]; then
    sudo sh -c "echo 'extension=pgsql_mapper.so' > \"$INI_DIR/20-pgsql_mapper.ini\""
else
    sudo sh -c 'echo "extension=pgsql_mapper.so" > /etc/php8/conf.d/20-pgsql_mapper.ini'
fi

php -r 'echo "pgsql_mapper loaded: " . (extension_loaded("pgsql_mapper") ? "yes" : "no") . "\n";'
