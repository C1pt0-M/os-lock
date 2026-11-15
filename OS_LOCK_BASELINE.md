# OS 锁项目：源码基线与来源

## 上游来源

- 项目：Linux stable kernel
- 官方仓库：https://git.kernel.org/pub/scm/linux/kernel/git/stable/linux-stable.git
- 获取分支：`linux-5.10.y`
- 固定提交：`3a323bc22b881a5bdc1646080f93f5ecb22850bf`
- 内核版本：`5.10.274`

本项目的首个私有快照提交导入上述版本的原始源码，不代表本团队原创实现了 Linux。原有 `COPYING`、`LICENSES/` 及源码中的版权和许可声明保持不变；具体文件以其原有许可为准。

`gitee-work` 是用于私有备份和后续实验提交的独立快照分支，不包含上游此前的完整提交链。原来的 `lock-work` 分支及 `baseline-5.10.274` 标签保留上游基线引用；`upstream` 远程用于追溯官方来源。后续内核修改须逐次提交并说明增量贡献。

## 已验证的开发环境

- Windows 11 + Ubuntu 22.04 WSL2
- GCC 11.4.0，GNU Make 4.3
- QEMU 6.2.0，使用 KVM 加速
- 客体配置：4 个虚拟 CPU、2 GiB 内存
- 客体已启动的版本：`5.10.274-baseline`
- 客体当前时钟源：`kvm-clock`
- 基线内核配置：本目录的 `OS_LOCK_BASELINE.config`
- 已启用普通内核抢占和 PI-futex；此配置不是 PREEMPT_RT 实时内核

基线配置包含 KVM 客体支持。此前未启用 KVM 客体支持、回退至 HPET 的启动记录不应与该配置混为同一性能基线。

## 构建与测试边界

当前本机使用源码目录外的 `../build-baseline/` 保存构建产物；`bzImage`、initramfs 和构建日志不纳入本次源码快照。后续实验应使用独立构建目录，避免覆盖已验证的基线镜像。

目前运行过的 `bench.c`、`bench_sleep.c` 是指导老师提供的非官方测试，尚未纳入本次快照。它们通过用户态 PI mutex 在竞争时进入 PI-futex/rt_mutex 路径；现有结果只用于初步验证和观测，不能单独证明内核改进或硬实时保证。后续应保存测试源码、编译参数、实际调度配置及重复运行记录。
