#include <assert.h>
#include <stdint.h>
#include <stdlib.h>

#include "error.h"
#include "nt.h"
#include "reflock.h"
#include "util.h"
#include "win.h"

/* clang-format off */
static const uint32_t _REF          = 0x00000001;
static const uint32_t _REF_MASK     = 0x0fffffff;
static const uint32_t _DESTROY      = 0x10000000;
static const uint32_t _DESTROY_MASK = 0xf0000000;
static const uint32_t _POISON       = 0x300DEAD0;
/* clang-format on */

static HANDLE _keyed_event = NULL;

int reflock_global_init(void) {
  NTSTATUS status =
      NtCreateKeyedEvent(&_keyed_event, ~(ACCESS_MASK) 0, NULL, 0);
  if (status != STATUS_SUCCESS)
    return_set_error(-1, RtlNtStatusToDosError(status));
  return 0;
}

void reflock_init(reflock_t* reflock) {
  reflock->state = 0;
}

static void _signal_event(void* address) {
  NTSTATUS status = NtReleaseKeyedEvent(_keyed_event, address, FALSE, NULL);
  if (status != STATUS_SUCCESS)
    abort();
}

static void _await_event(void* address) {
  NTSTATUS status = NtWaitForKeyedEvent(_keyed_event, address, FALSE, NULL);
  if (status != STATUS_SUCCESS)
    abort();
}

static inline uint32_t _sync_add_and_fetch(volatile uint32_t* target,
                                           uint32_t value) {
  static_assert(sizeof(*target) == sizeof(long), "");
  return (uint32_t) InterlockedAdd((volatile long*) target, (long) value);
}

static inline uint32_t _sync_fetch_and_set(volatile uint32_t* target,
                                           uint32_t value) {
  static_assert(sizeof(*target) == sizeof(long), "");
  return (uint32_t) InterlockedExchange((volatile long*) target, (long) value);
}

void reflock_ref(reflock_t* reflock) {
  uint32_t state = _sync_add_and_fetch(&reflock->state, _REF);
  unused_var(state);
  assert((state & _DESTROY_MASK) == 0); /* Overflow or destroyed. */
}

void reflock_unref(reflock_t* reflock) {
  uint32_t state = _sync_add_and_fetch(&reflock->state, 0 - _REF);
  uint32_t ref_count = state & _REF_MASK;
  uint32_t destroy = state & _DESTROY_MASK;

  unused_var(ref_count);
  unused_var(destroy);

  if (state == _DESTROY)
    _signal_event(reflock);
  else
    assert(destroy == 0 || ref_count > 0);
}

void reflock_unref_and_destroy(reflock_t* reflock) {
  uint32_t state = _sync_add_and_fetch(&reflock->state, _DESTROY - _REF);
  uint32_t ref_count = state & _REF_MASK;

  assert((state & _DESTROY_MASK) ==
         _DESTROY); /* Underflow or already destroyed. */

  if (ref_count != 0)
    _await_event(reflock);

  state = _sync_fetch_and_set(&reflock->state, _POISON);
  assert(state == _DESTROY);
}
