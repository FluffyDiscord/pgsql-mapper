#!/bin/bash
set -e

cd "$(dirname "$0")"

make clean 2>/dev/null || true
make

sudo cp modules/pgsql_mapper.so /usr/lib64/php8/extensions/
sudo sh -c 'echo "extension=pgsql_mapper.so" > /etc/php8/conf.d/20-pgsql_mapper.ini'

php -r 'echo "pgsql_mapper loaded: " . (extension_loaded("pgsql_mapper") ? "yes" : "no") . "\n";'
