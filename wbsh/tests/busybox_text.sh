#!/bin/bash
# The text, encoding, hash and calculator commands added to match busybox,
# plus the printf / echo fixes that came with them.

echo "=== printf"
printf '%b|\n' 'a\tb'
printf -v X '%s-%d' hi 0x10
echo "[$X]"
printf '%d %d %q\n' "'A" 010 'a b'
printf '%5s|%-5s|%%|%*d|\n' ab cd 4 7
printf '\101\x42\n'
printf '%b' 'stop\cnever'
echo
echo -e 'p\cq'
echo
echo --

echo "=== head / tail"
seq 1 10 > _bb_seq.txt
head -n -7 _bb_seq.txt | tr '\n' ' '
echo
head -c 5 _bb_seq.txt | od -c
tail -n +8 _bb_seq.txt | tr '\n' ' '
echo
tail -c 6 _bb_seq.txt | tr '\n' ' '
echo
head -2 _bb_seq.txt _bb_seq.txt

echo "=== egrep / fgrep"
echo hello | egrep 'l+o'
echo 'a.b' | fgrep '.'

echo "=== join / tsort / strings / shuf"
printf '1 apple\n2 banana\n3 cherry\n' > _bb_j1
printf '1 red\n3 dark\n4 green\n' > _bb_j2
join _bb_j1 _bb_j2
join -a2 -o 0,1.2,2.2 -e NONE _bb_j1 _bb_j2
printf 'a b\nb c\nc d\nx y\n' | tsort | tr '\n' ' '
echo
printf 'hello\001\002world!!\377ab\n' | strings
seq 5 | shuf | sort -n | tr '\n' ' '
echo
rm -f _bb_seq.txt _bb_j1 _bb_j2

echo "=== factor / cal / getopt"
factor 12 97 600851475143
cal 2 2024
getopt -o ab:c:: -l alpha,beta: -- -a -bX -c --beta=4 file1 --alp 'x y'
eval set -- "$(getopt -o v -l verbose -- --verbose 'a b')"
echo "[$1][$2][$3]"

echo "=== encodings"
printf 'foobar' | base32
printf 'MZXW6YTBOI======' | base32 -d
echo
printf 'Cat' | uuencode x.txt
printf 'Cat' | uuencode x.txt | uudecode -o -
echo
printf 'abc\n' | od -An -tx1
printf 'abcdefgh' | od -t x1 -t c
printf 'hello world\n' | hexdump -C
printf 'hello\n' | hexdump

echo "=== hashes"
printf abc | sha384sum
printf abc | sha3sum
printf abc | sha3sum -a 256
printf abc | cksum
printf abc | crc32
printf abc | sum
printf abc > _bb_a.txt
sha256sum _bb_a.txt > _bb_sums
sha256sum -c _bb_sums
rm -f _bb_a.txt _bb_sums

echo "=== dc / ipcalc / iconv"
dc -e '2 3 + p 10 3 / p 2 k 10 3 / p 2 10 ^ p'
ipcalc 192.168.10.77/20
printf 'Žluť' | iconv -f UTF-8 -t CP1250 | od -An -tx1
printf 'aéb' | iconv -c -t ASCII
echo

echo "=== grep"
printf 'a+b\naab\nabab\none two\nphone\n' > _bb_g.txt
grep 'a+b' _bb_g.txt
grep -E 'a+b' _bb_g.txt
grep '\(ab\)\1' _bb_g.txt
grep -ow 'one' _bb_g.txt
grep -o 'a\+' _bb_g.txt | sort | uniq -c
grep -n -A1 'aab' _bb_g.txt
grep -c -v 'a' _bb_g.txt
grep -e one -e phone -x _bb_g.txt
rm -f _bb_g.txt

echo "=== sed"
seq 1 6 | sed '2,4d'
seq 1 6 | sed -n '/3/,+1p'
seq 1 6 | sed '$!N;s/\n/ /'
seq 1 5 | sed -n '1!G;h;$p'
printf 'hello world\n' | sed 's/\(hello\) \(world\)/\2 \1/'
printf 'hello world\n' | sed -E 's/(\w+) (\w+)/\U\2\E \1/'
printf 'a.b.c\n' | sed 'y/./,/'
seq 1 3 | sed '2i\
inserted'
seq 1 3 | sed '/2/{s/2/two/;p}'
seq 1 4 | sed -n '0~2p'
printf 'xay\n' | sed 's/x*/-/g'

echo "=== env"
export BB_A=1
env BB_A=2 printenv BB_A
env -u BB_A printenv BB_A || echo "unset for the child"
echo "parent still $BB_A"

echo "=== done"
