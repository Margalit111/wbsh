#!/bin/bash
# File, archive and compression commands added to match busybox. The
# .xz / .lzma / .bz2 fixtures were written by the reference GNU tools, so
# the decoders are checked against independent encoders.

D=_bbf_tmp
rm -rf $D 2>/dev/null
mkdir -p $D
cd $D || exit 1
seq 1 200 > s.txt

echo "=== generator devices"
head -c 8 /dev/zero | od -An -tx1
head -c 16 /dev/urandom | wc -c
echo discarded > /dev/zero && echo "write to /dev/zero ok"

echo "=== dd / split / truncate"
dd if=/dev/zero of=z.bin bs=1k count=4 status=none
wc -c < z.bin
printf 'hello world' | dd bs=1 skip=6 count=3 conv=ucase status=none
echo
split -l 80 s.txt part_
ls part_*
cat part_* | cmp - s.txt && echo "split pieces rejoin"
truncate -s 100 t.bin
truncate -s +50 t.bin
wc -c < t.bin

echo "=== dos2unix / unix2dos"
printf 'a\r\nb\r\n' > crlf.txt
dos2unix crlf.txt
od -c crlf.txt
unix2dos crlf.txt
od -c crlf.txt

echo "=== link / unlink"
link s.txt hard.txt && cmp s.txt hard.txt && echo "hard link ok"
unlink hard.txt
[ -e hard.txt ] || echo "unlinked"

echo "=== patch"
seq 1 20 > p_old.txt
sed -e 's/^5$/five/' -e '15d' p_old.txt > p_new.txt
diff -u p_old.txt p_new.txt > p.diff
cp p_old.txt work.txt
patch work.txt < p.diff
cmp work.txt p_new.txt && echo "patched"
patch -R -s work.txt < p.diff
cmp work.txt p_old.txt && echo "reversed"

echo "=== diff -r / ls -R"
mkdir -p da/sub db/sub
echo 1 > da/x
echo 2 > db/x
echo same > da/sub/s
echo same > db/sub/s
echo only > da/sub/q
diff -r da db
ls -R da

echo "=== compression round trips"
gzip -c s.txt | gunzip | cmp - s.txt && echo "gzip ok"
bzip2 -c s.txt | bunzip2 | cmp - s.txt && echo "bzip2 ok"
cp s.txt k.txt
bzip2 -k k.txt && [ -f k.txt ] && [ -f k.txt.bz2 ] && echo "bzip2 -k kept the input"

echo "=== reference decoders"
echo '/Td6WFoAAATm1rRGAgAhARYAAAB0L+Wj4AKzANVdABiCgo8iTvimVffwmaUlDZBFkVpRtJvKrNwFMuyFUp+xSG3v3OhLuWG64JxTf5jIqVQO/D0q0wbXZkQ9VmSmzT3Xxx9E9BjfAgs/4sbGe+Hnf3lEHHm3Qq5luBu1DoThmoIGIzlaf3KqQ/qlmh+SwL5FcXlK1ZPGkAo5rZxlhyq2GjjH4ZMXytA+CeqwUvAQYCbAXl8QkQ25eXh9l0Jg0KY4d+Ej6LsoT4hSLXVYJj9m/eKc69OvYdk1ev/7YrE8fJMc8mS4TRyFKrFcAAY2AUrrpRk/YAAAAAAqY7pSQQQm/AAB8QG0BQAA9ywgtLHEZ/sCAAAAAARZWg==' | base64 -d > s.xz
xzcat s.xz | cmp - s.txt && echo "xz ok"
echo 'XQAAgAD//////////wAYgoKPIk74plX38JmlJQ2QRZFaUbSbyqzcBTLshVKfsUht79zoS7lhuuCcU3+YyKlUDvw9KtMG12ZEPVZkps0918cfRPQY3wILP+LGxnvh5395RBx5t0KuZbgbtQ6E4ZqCBiM5Wn9yqkP6pZofksC+RXF5StWTxpAKOa2cZYcqtho4x+GTF8rQPgnqsFLwEGAmwF5fEJENuXl4fZdCYNCmOHfhI+i7KE+IUi11WCY/Zv3inOvTr2HZNXr/+2KxPHyTHPJkuE0chSqxXAAGNgFK66+ewWT/+5aaoA==' | base64 -d > s.lzma
lzcat s.lzma | cmp - s.txt && echo "lzma ok"
echo 'QlpoOTFBWSZTWXBO73QAAHlIAAAQf+BAAbBwAgGmmgip+gbeqpCAaaaCn+qpIb/VP9VUv6r3zvsj6LAMNAPDYHQ4DgOhsDw0AwsAigAxTQDw2B0OA4DobA8NAMLAIoAJUqSSSMzMzMzJX6l1oB9JJJJF1+4d7w73h3vDveHe8O94d7w73h3vLyVVWtttt7uAAAXleVVbW223v0AAC8q+VVtrbbe/wAAF5VflVttbbe/oAAF5VX7Vttrbe/gAAF5VV/Vtttbe/AAALyqq+2222t74AABzy+VVX33bbbaAADj/F3JFOFCQcE7vdA==' | base64 -d > s.bz2
bzcat s.bz2 | cmp - s.txt && echo "bzip2 reference ok"
echo 'H52MMRTIUDBDAQ0FNRTYUHBDAQ4FORTEgCExYIyBMQrGOBgjYYyFMRrGeBgjogyKMgLKGCijoIyDMhLKWCijoYyHMiLOoDgj4IyBMwrOODgj4YyFMxrOeDgjIg2KNALSGEijII2DNBLSWEijIY2HNCLWoFgjYI2BNQrWOFgjYY2FNRrWeFgjog2KNgLaGGijoI2DNhLaWGijoY2HNiLeoHgj4I2BNwreOHgj4Y2FNxreeHgjIg6KOALiGIijII6DOBLiWIijIY6HOCLmoJgjYI6BOQrmOJgjYY6FORrmeJgj4kSKEy3CwAhDIwyOMDzCAAlDJAySMIxPrGjxYkWNGyt6/FhR5MiKxk9KTLkeY8v1HGOuB1lzPcmcEnfmt/gzv8ah+Xl0VH4iLZWfcU9JFJWCGFWlIEdZKQhSVwqSFJZEY2Fo0VkYarQWhh69haFIc2Fo3F0S5ZUiRn2lyFFgKYJUWIokJSbRYjda9NiNGk12o0eX3SjSZjca95lEoSGJUWlIcpQakiC1hiRJsUk0m5UW3WalRrtZ6dFvVoo0nJUmwUAR' | base64 -d > s.Z
uncompress -c s.Z | cmp - s.txt && echo "compress ok"

echo "=== tar"
mkdir -p tree/sub
echo hello > tree/a.txt
echo deep > tree/sub/b.txt
tar -czf t.tgz tree
tar -tzf t.tgz
mkdir out
tar -xf t.tgz -C out --strip-components=1
ls out out/sub
tar -xOf t.tgz tree/a.txt
tar -tf t.tgz --exclude='*.txt'
echo '/Td6WFoAAATm1rRGAgAhARYAAAB0L+Wj4Cf/AIRdADoLvBunjDRE4cIHb96UUvZgZA+mgpP6Ovhkh1WARpXW5PdrKh4m7w8JCzflIgmJbd27T+SlBqsgyUbhiuag2E3cfiXn4GaPhaFmtmQ4qFPHvbenIBq6WXPpVkhWFNikb56N8o+W4b+76YWSz1u8gWaqHUvMLR9wzqHkCVu2KfXFt80bAACZju7RWUcEsAABoAGAUAAACHTMZrHEZ/sCAAAAAARZWg==' | base64 -d > ref.txz
mkdir ref
tar -xf ref.txz -C ref
cat ref/t/a.txt

echo "=== cpio"
find tree | sort | cpio -o > t.cpio 2>/dev/null
cpio -t < t.cpio 2>/dev/null
mkdir cp_out
cd cp_out
cpio -id < ../t.cpio 2>/dev/null
cat tree/sub/b.txt
cd ..

cd ..
rm -rf $D
echo "=== done"
