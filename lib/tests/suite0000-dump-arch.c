// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Prints attributes like integer widths and endianness to record the arch being
 * tested. The actual CPU arch being tested is not identified and the code is
 * arch agnostic for future proofing.
 *
 * The program should always return zero.
 */
#include <stdio.h>
#include <stdlib.h>
#include <inttypes.h>
#include <limits.h>
#include <assert.h>
#include <unistd.h>
#include <signal.h>
#include <sys/wait.h>
#include <config.h>

#ifdef WORDS_BIGENDIAN
#define IS_BIGENDIAN	(1)
#else
#define IS_BIGENDIAN	(0)
#endif

/*
 * Intentionally access the unaligned memory.
 *
 * Do not make this static otherwise the compiler could align the memory access
 * pattern.
 */
__attribute__((noinline))
int do_misaligned_access(void *m)
{
	volatile int *a = (int *)((char *)m + 0);
	volatile int *b = (int *)((char *)m + sizeof(int));

	return *a - *b;
}

static void handle_sigbus(int signum)
{
	_exit(3);
}

int main(void)
{
	pid_t child;
	int wstatus = 0;
	int child_ec;

	printf(	"\n"
		"CHAR_BIT:  %d\n"
		"BIGENDIAN: %d\n"
		"uintptr_t: %zu\n"
		"size_t:    %zu\n"
		"short:     %zu\n"
		"int:       %zu\n"
		"long:      %zu\n" /* for detecting LLP64 model(MSVC) */
		"long long: %zu\n",
		CHAR_BIT,
		IS_BIGENDIAN,
		sizeof(uintptr_t),
		sizeof(size_t),
		sizeof(short),
		sizeof(int),
		sizeof(long),
		sizeof(long long));

	fflush(stdout);
	fflush(stderr);

	child = fork();
	if (child < 0) {
		perror("fork()");
		abort();
	} else if (child == 0) {
		struct sigaction sa = { .sa_handler = handle_sigbus, };
		struct __attribute__((packed)) {
			char stuff;
			int a;
			int b;
		} m_misaligned = {
			.a = 123,
			.b = 123,
		};
		struct __attribute__((packed)) {
			int a;
			int b;
		} m_aligned = {
			.a = 123,
			.b = 123,
		};

		/*
		 * Not really necessary to catch this, but modern distros with crash report daemon
		 * might detect the crash and complain. Let's not annoy the user.
		 */
		sigaction(SIGBUS, &sa, NULL);

		/*
		 * Call the function with both memory so that the compiler won't
		 * make only one aligned variant. As the function is globally
		 * visible, it has no choice but to generate unaligned variant.
		 * Aliasing a globally visible function is in violation of C
		 * specs(dlopen()).
		 */
		return
			do_misaligned_access(((char *)&m_misaligned) + 1) +
			do_misaligned_access(&m_aligned);
	}

	waitpid(child, &wstatus, 0);
	assert(WIFEXITED(wstatus));

	child_ec = WEXITSTATUS(wstatus);
	printf("Unaligned access: %s(%d)\n", child_ec == 0 ? "ABLE" : "CRASH", child_ec);

	return 0;
}
