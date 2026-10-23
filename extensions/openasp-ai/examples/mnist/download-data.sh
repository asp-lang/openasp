#!/bin/sh
set -eu

destination="${1:-data}"
base_url="https://ossci-datasets.s3.amazonaws.com/mnist"
archive=
temporary=

cleanup() {
    rm -f "${archive:-}" "${temporary:-}"
}
trap cleanup EXIT HUP INT TERM

mkdir -p "$destination"

for file in \
    train-images-idx3-ubyte \
    train-labels-idx1-ubyte \
    t10k-images-idx3-ubyte \
    t10k-labels-idx1-ubyte
do
    output="$destination/$file"
    if [ ! -f "$output" ]; then
        archive="$destination/.$file.gz.$$"
        temporary="$destination/.$file.$$"
        curl --fail --location --retry 3 --output "$archive" "$base_url/$file.gz"
        gzip -dc "$archive" >"$temporary"
        mv "$temporary" "$output"
        rm -f "$archive"
        archive=
        temporary=
    fi
done

trap - EXIT HUP INT TERM
printf 'MNIST IDX files are ready in %s\n' "$destination"
