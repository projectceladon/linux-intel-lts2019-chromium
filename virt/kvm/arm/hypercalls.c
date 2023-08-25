// SPDX-License-Identifier: GPL-2.0
// Copyright (C) 2019 Arm Ltd.

#include <linux/arm-smccc.h>
#include <linux/kvm_host.h>
#include <linux/cpufreq.h>
#include <linux/sched.h>
#include <uapi/linux/sched/types.h>

#include <asm/kvm_emulate.h>

#include <kvm/arm_hypercalls.h>
#include <kvm/arm_psci.h>

static void kvm_sched_get_cur_cpufreq(struct kvm_vcpu *vcpu, long *val)
{
	unsigned long ret_freq;

	ret_freq = cpufreq_get(task_cpu(current));

	*(unsigned long *)val = ret_freq;
}

int kvm_hvc_call_handler(struct kvm_vcpu *vcpu)
{
	u32 func_id = smccc_get_function(vcpu);
	long val = SMCCC_RET_NOT_SUPPORTED, val2 = 0;
	u32 feature;
	gpa_t gpa;

	switch (func_id) {
	case ARM_SMCCC_VERSION_FUNC_ID:
		val = ARM_SMCCC_VERSION_1_1;
		break;
	case ARM_SMCCC_ARCH_FEATURES_FUNC_ID:
		feature = smccc_get_arg1(vcpu);
		switch (feature) {
		case ARM_SMCCC_ARCH_WORKAROUND_1:
			switch (kvm_arm_harden_branch_predictor()) {
			case KVM_BP_HARDEN_UNKNOWN:
				break;
			case KVM_BP_HARDEN_WA_NEEDED:
				val = SMCCC_RET_SUCCESS;
				break;
			case KVM_BP_HARDEN_NOT_REQUIRED:
				val = SMCCC_ARCH_WORKAROUND_RET_UNAFFECTED;
				break;
			}
			break;
		case ARM_SMCCC_ARCH_WORKAROUND_2:
			switch (kvm_arm_have_ssbd()) {
			case KVM_SSBD_FORCE_DISABLE:
			case KVM_SSBD_UNKNOWN:
				break;
			case KVM_SSBD_KERNEL:
				val = SMCCC_RET_SUCCESS;
				break;
			case KVM_SSBD_FORCE_ENABLE:
			case KVM_SSBD_MITIGATED:
				val = SMCCC_RET_NOT_REQUIRED;
				break;
			}
			break;
		case ARM_SMCCC_ARCH_WORKAROUND_3:
			switch (kvm_arm_get_spectre_bhb_state()) {
			case SPECTRE_VULNERABLE:
				break;
			case SPECTRE_MITIGATED:
				val = SMCCC_RET_SUCCESS;
				break;
			case SPECTRE_UNAFFECTED:
				val = SMCCC_ARCH_WORKAROUND_RET_UNAFFECTED;
				break;
			}
			break;
		case ARM_SMCCC_HV_PV_TIME_FEATURES:
			val = SMCCC_RET_SUCCESS;
			break;
		}
		break;
	case ARM_SMCCC_HV_PV_TIME_FEATURES:
		val = kvm_hypercall_pv_features(vcpu);
		break;
	case ARM_SMCCC_HV_PV_TIME_ST:
		gpa = kvm_init_stolen_time(vcpu);
		if (gpa != GPA_INVALID)
			val = gpa;
		break;
	case ARM_SMCCC_VENDOR_HYP_KVM_FEATURES_FUNC_ID:
		val = BIT(ARM_SMCCC_KVM_FUNC_FEATURES);
		val2 |= BIT(ARM_SMCCC_KVM_FUNC_GET_CUR_CPUFREQ % 32);
		break;
	case ARM_SMCCC_VENDOR_HYP_KVM_GET_CUR_CPUFREQ_FUNC_ID:
		kvm_sched_get_cur_cpufreq(vcpu, &val);
		break;
	default:
		return kvm_psci_call(vcpu);
	}

	smccc_set_retval(vcpu, val, 0, val2, 0);
	return 1;
}
