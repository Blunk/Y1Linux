"""Y1 D7c: stock's VPROC sleep voltage around a system sleep (docs/planning/Y1-POWER-PLAN.md §19).

Source placement only; the policy itself (the eight stock fields, the plan's refusals, the restore order) is the
Y2's vproc-sleep-policy.h and is tested in test_vproc_sleep.py.
"""
import json
from pathlib import Path
import unittest

ROOT = Path(__file__).resolve().parents[1]
DEEP_IDLE = ROOT / 'kernel/y1/deep-idle.c'


def function(source, signature):
    start = source.index(signature)
    return source[start:source.index('\n}\n', start) + 3]


class Placement(unittest.TestCase):
    def setUp(self):
        self.src = DEEP_IDLE.read_text()

    def test_shared_policy_is_built_with_the_y1_file(self):
        manifest = json.loads((ROOT / 'kernel/patches-y1/manifest.json').read_text())
        added = {entry['path']: entry['source'] for entry in manifest['added']}
        self.assertEqual(added['drivers/soc/mediatek/vproc-sleep-policy.h'], 'kernel/platform/vproc-sleep-policy.h')
        self.assertIn('#include "vproc-sleep-policy.h"', self.src)
        # One table: the Y1 file writes the shared fields, never its own copy of them.
        self.assertNotIn('0x8097', self.src)
        self.assertIn('y2_vproc_sleep_plan(&vproc_sleep', self.src)

    def test_applied_at_prepare_late_and_restored_at_wake_never_in_enter(self):
        ops = self.src[self.src.index('static const struct platform_suspend_ops sleep_ops'):]
        ops = ops[:ops.index('};')]
        self.assertIn('.prepare_late = sleep_prepare_late', ops)
        self.assertIn('.wake = sleep_platform_wake', ops)
        wake = function(self.src, 'static void sleep_platform_wake(void)')
        self.assertLess(wake.index('sleep_vproc_wake();'), wake.index('sleep_wake();'))
        # The mainline PMIC wrapper's regmap sleeps: no PMIC access with interrupts off.
        for name in ('static int sleep_enter(', 'static int sleep_spm(void)', 'static int sleep_finish('):
            body = function(self.src, name)
            self.assertNotIn('regmap', body, name)
            self.assertNotIn('vproc', body, name)

    def test_never_refuses_the_sleep(self):
        late = function(self.src, 'static int sleep_prepare_late(void)')
        self.assertNotIn('return ret', late)
        self.assertNotIn('return -', late)

    def test_deep_idle_refuses_while_armed(self):
        admit = function(self.src, 'static int dp_admit(')
        self.assertIn('if (READ_ONCE(vproc_armed))\n\t\treturn DP_SLEEP;', admit)
        self.assertLess(admit.index('vproc_armed'), admit.index('smp_processor_id()'))
        late = function(self.src, 'static int sleep_prepare_late(void)')
        # Armed before the first PMIC write, cleared only after a verified restore.
        self.assertLess(late.index('WRITE_ONCE(vproc_armed, true)'), late.index('vproc_apply(pmic)'))
        wake = function(self.src, 'static void sleep_vproc_wake(void)')
        self.assertLess(wake.index('vproc_restore(pmic)'), wake.index('WRITE_ONCE(vproc_armed, false)'))
        failed = wake[wake.index('if (ret) {'):wake.index('vproc_restores++')]
        self.assertIn('sleep_break("vproc_restore")', failed)
        self.assertNotIn('WRITE_ONCE(vproc_armed', failed)

    def test_requires_srclken_hardware_mode_and_verifies_the_voltage(self):
        plan = function(self.src, 'static int vproc_plan(')
        self.assertIn('PMIC_SRCLKEN_HW_MODE', plan)
        self.assertIn('#define PMIC_TOP_CKCON0 0x120', self.src)
        self.assertIn('#define PMIC_SRCLKEN_HW_MODE BIT(4)', self.src)
        for name in ('static int vproc_apply(', 'static int vproc_restore('):
            body = function(self.src, name)
            self.assertIn('PMIC_NI_VPROC_VOSEL, &vproc_actual', body, name)
            self.assertIn('Y2_VPROC_ACTIVE_SELECTOR', body, name)
        restore = function(self.src, 'static int vproc_restore(')
        self.assertIn('y2_vproc_restore_index(n)', restore)

    def test_state_reports_the_step_with_unique_keys(self):
        show = function(self.src, 'static ssize_t state_show(')
        line = show[show.index('"vproc_sleep='):]
        line = line[:line.index('\\n"')]
        keys = [word.split('=')[0] for word in line.strip('"').split()]
        self.assertEqual(len(keys), len(set(keys)))
        self.assertTrue(all(key.startswith('vproc_') for key in keys))


if __name__ == '__main__':
    unittest.main()
