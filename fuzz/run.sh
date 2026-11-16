#!/usr/bin/env bash
# Run the fuzzer (fuzz/fuzz.c) against a staged libpandoc, in child
# processes on several cores, until a time is up; keep what fails.
#
#   fuzz/run.sh [-j JOBS] [-m MINUTES] [-n COUNT] [-t SECONDS] [-v KB] [-T EVERY] PREFIX OUT
#
# PREFIX is a staged libpandoc (scripts/build.sh's dist: include/, lib/).
# Each child runs COUNT iterations (default 100) from its own seed, under
# a time limit (-t, default 600 s) and a memory cap (-v, ulimit -v in KB,
# default 8000000: GHC reserves its heap's address space at start, about
# half of it), with standard input /dev/null and output discarded;
# every EVERYth child (-T, default 20; 0: none) runs with every call
# untrusted (fuzz -u) under strace, which must see no file of the canary
# opened. SEED=N starts the seeds at N (default: from the clock).
#
# OUT/log has a line per child that failed and a summary; a failure's
# seed, standard error, inputs (fuzz -D) and replay command are kept in
# OUT/found/CLASS/SEED/. Classes: violation (an invariant, fuzz exits 3),
# crash (a signal), exit-N (the process exited otherwise), hang (an
# iteration over 90 s, fuzz exits 4; or the child's time limit), oom (out
# of memory: exit 251, or 1 when no thread could be made, or Lua's panic,
# then an abort or a segfault: by design, kept apart so that it buries
# nothing), canary-open (the trace).
set -euo pipefail
jobs=$(($(nproc) / 3)) minutes=60 count=100 secs=600 mem=8000000 every=20
while getopts j:m:n:t:v:T: o; do
	case $o in
	j) jobs=$OPTARG ;; m) minutes=$OPTARG ;; n) count=$OPTARG ;;
	t) secs=$OPTARG ;; v) mem=$OPTARG ;; T) every=$OPTARG ;;
	*) exit 2 ;;
	esac
done
shift $((OPTIND - 1))
prefix=$(cd "$1" && pwd)
mkdir -p "$2"
out=$(cd "$2" && pwd)
here=$(cd "$(dirname "$0")" && pwd)
cc -O1 -g -Wall -pthread -o "$out/fuzz" "$here/fuzz.c" -I"$prefix/include" \
	-L"$prefix/lib" -lpandoc -Wl,-rpath,"$prefix/lib"
base=${SEED:-$(($(date +%s) % 100000 * 1000))}
end=$((SECONDS + minutes * 60))
if ! command -v strace >/dev/null; then every=0; fi
# many children at once: a few threads each (the fuzzer sets it too)
export LIBPANDOC_NUM_THREADS=${LIBPANDOC_NUM_THREADS:-4}
: >"$out/log"

worker() {
	local w=$1 seed=$(($1 + base)) n=0 dir=$out/work/$1
	while ((SECONDS < end)); do
		rm -rf "$dir"
		mkdir -p "$dir/tmp"
		local flags=() traced=0
		if ((every > 0 && n % every == every - 1)); then flags=(-u) traced=1; fi
		set +e
		(
			cd "$dir" && ulimit -v "$mem" && export TMPDIR=$dir/tmp || exit 99
			if ((traced)); then
				exec timeout -k 10 "$secs" strace -f -qq -e trace=openat,open,access \
					-o "$dir/trace" "$out/fuzz" -u "$seed" "$count"
			fi
			exec timeout -k 10 "$secs" "$out/fuzz" "$seed" "$count"
		) </dev/null >/dev/null 2>"$dir/stderr"
		local st=$? class=
		set -e
		if ((st == 0)) && ((traced)) &&
			sed -n '/fuzz-setup-done/,$p' "$dir/trace" | grep -v ' access(' |
			grep -qE 'fuzz-[A-Za-z0-9]{6}/c/'; then
			class=canary-open
		elif ((st == 0)); then :
		elif ((st == 3)); then class=violation
		elif { ((st == 251)) && grep -q 'out of memory' "$dir/stderr"; } ||
			{ ((st == 1)) && grep -q 'failed to create OS thread' "$dir/stderr"; } ||
			{ ((st == 134 || st == 139)) && grep -q 'PANIC: .*(not enough memory)' "$dir/stderr"; }; then class=oom
		elif ((st == 4 || st == 124 || st == 137)); then class=hang
		elif ((st > 128)); then class=crash
		else class=exit-$st
		fi
		grep -h '^calls ' "$dir/stderr" | cut -d' ' -f2 >>"$out/calls.$w" || true
		if [[ -n $class ]]; then
			local k keep=$out/found/$class/$seed
			k=$(grep '^iter ' "$dir/stderr" | tail -1 | cut -d' ' -f2)
			if [[ $class == canary-open ]]; then # the iteration of the first open
				k=$(awk '/fuzz-setup-done/ { on = 1 }
					/fuzz-iter-/ { match($0, /fuzz-iter-[0-9]+/); k = substr($0, RSTART + 10, RLENGTH - 10) }
					on && /openat/ && /fuzz-[A-Za-z0-9]+\/c\// { print k; exit }' "$dir/trace")
			fi
			mkdir -p "$keep"
			cp "$dir/stderr" "$keep/"
			if ((traced)); then
				sed -n '/fuzz-setup-done/,$p' "$dir/trace" | grep -E 'fuzz-[A-Za-z0-9]{6}/c/' |
					head -50 >"$keep/canary-opens" || true
			fi
			echo "fuzz ${flags[*]} $seed 1 ${k:-0}   # alone; the whole child: fuzz ${flags[*]} $seed $count" >"$keep/replay"
			(cd "$keep" && TMPDIR=$dir/tmp "$out/fuzz" -D ${flags[@]+"${flags[@]}"} "$seed" 1 "${k:-0}" \
				>/dev/null 2>&1 </dev/null) || true
			echo "$(date +%T) $class seed=$seed iter=${k:-?} status=$st" \
				"$(grep -m1 VIOLATION "$dir/stderr" | cut -c1-200)" >>"$out/log"
		fi
		seed=$((seed + jobs))
		n=$((n + 1))
		echo x >>"$out/children.$w"
	done
	rm -rf "$dir"
}

for ((w = 0; w < jobs; w++)); do worker "$w" & done
wait
{
	echo "children: $(cat "$out"/children.* 2>/dev/null | wc -l)," \
		"calls (in children that ended normally): $(cat "$out"/calls.* 2>/dev/null | awk '{s += $1} END {print s + 0}')"
	for c in "$out"/found/*/; do
		[[ -d $c ]] && echo "$(basename "$c"): $(find "$c" -mindepth 1 -maxdepth 1 -type d | wc -l)"
	done
} | tee -a "$out/log"
