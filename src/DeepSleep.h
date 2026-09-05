#pragma once

// Called by a bounded pre-sleep activity when its continuation has reached a
// terminal outcome. The main task owns the actual hardware teardown.
void completeDeferredDeepSleep(bool fromTimeout = false);
