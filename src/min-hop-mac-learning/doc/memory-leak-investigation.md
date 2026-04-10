# 内存增长问题调查记录

记录时间：2026-04-10

这份文档记录本轮内存增长问题的已确认结论、已做处理和后续验证口径。这里的“内存泄漏”按仿真项目的实际问题理解：不只包括经典的 `new` 后未释放，也包括某些统计或状态结构在当前仿真阶段不再需要、但仍然持续占用大量内存的情况。

## 1. 调查结论

本轮 50 层 `microAllReduce_50` 调查中，主要内存峰值不是由 RoCE 数据面包队列长期堆积造成的，而是由 AstraSim 收尾阶段的 CSV 统计路径触发。

关键路径是：

1. `Workload::report()` 在 `seprate_log` 开启时构造 `dimension_utilization` 统计数据。
2. 该路径调用 `generator->scheduler_unit->usage[i].report_percentage(10000)`。
3. `UsageTracker::report_percentage(10000)` 会从 0 tick 按 10000 tick 采样到当前 boosted tick。
4. 当 boosted tick 约为 `5e11` 时，单维会生成约 5000 万个采样点；两维接近 1 亿个 `std::list` 节点。
5. `CSVWriter::finalize_csv(...)` 的实际写文件逻辑已经被注释，但调用前的 `dims` 已经构造完成，而且旧签名按值传参还会产生额外拷贝风险。

因此，这个问题更准确地说是“已禁用 CSV 输出路径没有清理干净，仍在生成超大中间统计结构”，不是 RoCE packet runtime 本身的持续泄漏。

## 2. 已处理内容

当前版本做了以下处理：

1. `Workload` 中 CSV writer 创建路径继续保持禁用状态，并标记 `lja delete`。
2. `Workload::report()` 中 `dimension_utilization` 的 `dims` 构造和 `finalize_csv(dims)` 调用已保留为注释块，并标记 `lja delete`。
3. `Layer` 中原本已注释的 `EndToEnd->write_line/write_res` 保持禁用。
4. `Layer` 中仍可能通过 `EndToEnd->path` 写 `chart.html` 的 visual 分支也已注释，并标记 `lja delete`。
5. 本轮为定位问题临时加入的内存探针已清理，包括 `[MEM][PROBE]`、`[ASTRA][MEM]`、`/proc/self/status` 读取、主循环周期 probe，以及下层为 probe 服务的 `GetProbeSnapshot` 类接口。

注意：CSV 相关业务代码按“保留但注释掉”的方式处理，便于以后如果真的需要恢复 CSV 输出时重新设计，而不是彻底删除。

## 3. 验证结果

关闭 dimension utilization CSV 后，50 层模型复跑时：

1. 原先 `Workload::report:after_dimension_utilization` 处约 9 GB 的 RSS/HWM 峰值消失。
2. 进程内存保持在约 140 MB 量级。
3. RoCE/RDMA 侧动态计数在结束时归零。
4. 仍观察到 `generator_id=0` 的 `report()` 可能执行两次，但在 CSV 大对象不再构造后，它不再造成内存峰值。

临时内存探针清理后，小模型复跑验证：

1. `simulation.log` 中没有 `[MEM][PROBE]`。
2. `simulation.log` 中没有 `[ASTRA][MEM]`。
3. `simulation.log` 中没有 `rss_kb/hwm_kb/vmsize_kb/rss_mb/hwm_mb/vmsize_mb` 等探针字段。
4. 小模型日志降级后不再打印完整 `FLOW_TX_DONE / FLOW_RX_DONE` 明细；当前可直接确认 `REQ_DONE=144`、`total send count=144`。
5. 日志显示 `network drained`，没有新的显式失败；`MACTABLE ... fail=0` 不是错误。

## 4. 后续建议

短期内不要恢复当前形式的 dimension utilization CSV。若未来确实需要这类统计，建议重新设计为：

1. 按仿真总时长自适应采样间隔，避免固定 `10000` tick 在长仿真中生成海量样本。
2. 使用流式写出或有界缓冲，不要先构造完整 `std::list<std::list<...>>`。
3. 如果保留 `CSVWriter::finalize_csv`，参数至少应改成 `const&`，避免按值拷贝大结构。
4. 再评估 `Workload::check_for_sim_end()` 中 `report()` 执行两次的语义，但这不是本轮 CSV 内存峰值的主因。
