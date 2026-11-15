# Windows工作区文件候选：独立实现与使用边界

当前实现是Source平台层的连续会话文件候选owner，**已接入当前开发树HJSON目录Source**。本层用真实Windows通知找可能变化的路径；本层不读正文、不算SHA、不运行Myers，也不判断是否作者手改或是否删除UE资产。后续Source按选中文档读取真实版本、运行原Myers/parser/typed；不能把独立候选层的毫秒计时当成HJSON端到端耗时。

实现：[UeAgentWorkspaceChangeObserver.h](/D:/program/UE/GptProjectTest/Plugins/UeAgentInterface/Source/UeAgentInterface/Private/Core/HjsonWorkspace/UeAgentWorkspaceChangeObserver.h:12)、[同名cpp](/D:/program/UE/GptProjectTest/Plugins/UeAgentInterface/Source/UeAgentInterface/Private/Core/HjsonWorkspace/UeAgentWorkspaceChangeObserver.cpp:60)。同一cpp由原生standalone consumer直接编译，不是Python/watchdog原型或测试替身。

## 1. 开始与真正做的工作

`FObserver::Start(root,error)`只接受当前支持的Windows/NTFS普通目录；打开root递归观察和parent非递归观察，取得root文件身份，异步提交`ReadDirectoryChangesExW`后启动一个收集线程。parent用于发现root本身被删除/改名，root通知不会报告root自身变化。

启动没有初始目录遍历，也没有全文件名单、正文或hash读取。因此它只覆盖**成功Start之后这个连续会话**，不能推断启动前/关闭期间改过哪些文件。正式Source由显式full或首次初始化建立起点，再由实际成功publication提供同代文件表、pending集合及实际公开旧bytes；见§5。启动本身不会为旧目录自动建立这些内容。

每完成一批异步IO：核完成码/长度→逐条核记录边界→按相对路径合并候选→尝试用NTFS FileId配对rename两端→重新提交观察。原内核句柄在两次提交之间仍缓冲变化；发生溢出则停止，不枚举/轮询/hash弥补。

## 2. 返回的是哪些字段

| 字段 | 实际含义 |
| --- | --- |
| `FSnapshot.Status/Error` | Watching/Failed/Closed；错误含Code、Win32 NativeCode、Stage和Path。false不表示无修改 |
| `RootFileId/VolumeSerial` | 本会话root及卷绑定；FileId不是跨任意时间/卷永恒身份或内容证明 |
| `Candidates[].Path` | 通知所给、相对root的Windows路径拼写；保中文和大小写，不盲合并大小写不同的名字 |
| `FileId/ParentFileId` | 此路径最近观察记录的64位NTFS ID；不是原文件lease证明的128位ID。原子替换后可能是新物理文件ID |
| `Changes` | Added/Removed/Modified/RenameFrom/RenameTo的位集合；允许同路径既删又加，不伪造唯一业务“修改” |
| `Directory` | 此事件路径是目录；纯Modified可能只是目录属性/时间变化。目录增删/移动可能表示**子树失效前缀**：OS不保证逐报移入的全部文件。独立owner不枚举；当前严格Source对无publication子成员交接的目录结构事件明确停止，不自动递归扫描 |
| `Renames` | 收到可配对的同FileId旧/新名字；未配对仍保两端各自候选，不猜配对；跨root通常是进入/移出 |
| `First/LastSequence`、`ThroughSequence` | 本会话通知收集顺序和候选确认截点，仅用于防并发ack丢候选；与durable序号、作者已应用或workspace through无关 |
| `First/LastObservedNs` | 应用拿到内核完成后开始处理本批的单调时钟，不是内核生成通知时间、字节稳定时点或作者时间 |

同一文件连续修改合成一个dirty候选，Snapshot不会消费它；A未处理又来B时两者保留。Acknowledge旧snapshot只退休`LastSequence <= 截点`的候选；截点后再改的A不被清掉。调用者应在本层候选处理完成后确认，不能由此推进UE提交/作者消费前沿。

同路径的旧物理身份不会被当作新after；`FileId`只表示最近事件。需要原子替换前旧目的身份的消费者必须持有自己的正确旧版本，不能从这个压缩候选反推每次历史字节。rename链可保多个相关路径，移位不产生UE身份。

## 3. 调用示例

```cpp
namespace FC = UE::UAI::HjsonWorkspace::FileChanges;
FC::FObserver Watch;
FC::FError Error;
if (!Watch.Start(L"D:\\exact\\workspace", Error)) {
    // 输出Error.Stage/Path/Code/NativeCode；不换W/轮询/full扫描。
    return;
}
FC::FSnapshot Selected;
if (!Watch.Snapshot(Selected)) {
    // Failed要保选中材料/报断连续性；Closed也不能当clean。
    return;
}
for (const auto& Candidate : Selected.Candidates) {
    // Directory=true处理相应子树前缀；其它才是单路径候选。
    // 再由原Source读取真实版本/比较内容，不能直接转UE增删。
}
// 只有上面的候选处理成功才确认；新事件不会被这个旧截点吞掉。
Watch.Acknowledge(Selected.ThroughSequence, Error);
Watch.Close();
```

`Start/Close`由生命周期线程串行调用；Snapshot/Acknowledge可与收集线程并发。已经Start（包括Failed）再Start会明确AlreadyStarted，**不会静默替换会话丢掉pending**；先取所需错误/候选，再明确Close。Close发停止信号，由收集线程CancelIoEx并等待IO完成，再join，最后释放OVERLAPPED/buffer/句柄。没有用户回调在收集线程执行，没有native UObject指针。

## 4. 失败、适用性与不作出的保证

- API不存在/卷不是NTFS/打开失败/不支持通知/候选是reparse point：具体错误，**不自动换ReadDirectoryChangesW、USN、目录枚举或hash算法**。
- `BufferBytes`没有自行规定的1024字节下限或长度必须整除4。Win32要求的是**缓冲区地址**DWORD对齐，backing以DWORD数组分配；原请求长度原样传API，包括0，物理分配向上取整只为地址/存储，不改变请求大小。太小会真实丢通知或被OS拒绝，按实际错误处理，不返回clean。
- 异步完成零bytes或ERROR_NOTIFY_ENUM_DIR：sticky NotificationLost，保已收到候选但不能当完整名单，拒绝ack；需明确结束并重新建立观察，本实现不支持断点补段。
- root移除/rename/身份换绑：parent通知或Snapshot当前root属性核验触发RootChanged。每次Snapshot只有root属性打开，不逐个打开未改文件。
- parent观察也有缓冲：大量无关同级目录活动若使它丢通知，本会话同样明确失效；不承诺任意父目录噪声下持续可用，也不以忽略overflow继续假报完整。
- 短名/长名哪种由OS返回未指定；本实现保原拼写与FileId，没有完整8.3别名表。外部硬链接写入、已有reparse/别名树、ReFS/SMB、超长路径及断电均未验证，不宣称全覆盖。事件中的reparse候选明确拒绝，但启动不遍历所有后代证明没有reparse。
- 通知是候选而非byte diff：时间戳/属性变化也可能报Modified；真实写入缓存刷新、正在保存和通知延迟不能由“暂时没候选”排除。读稳定版本、系统发表/作者归因和最后并发字节保护由当前Source/publisher承担，通知本身不授予这些证明。
- 未实现USN或跨重启连续性；没有权限提升、服务、卷日志修改、公共命令或配置变更。

复杂度以真正收到的记录E、未确认候选C、rename R计：收集合并约O(E log C)；Snapshot复制O(C+R)并做常数个root属性操作；ack遍历本会话待确认集合。不是每次O(所有workspace文件)，也不承诺O(1)或任意写入率永不溢出。

官方依据：[ExW及溢出/卷支持](https://learn.microsoft.com/en-us/windows/win32/api/winbase/nf-winbase-readdirectorychangesexw)、[通知字段及64位ID/短长名](https://learn.microsoft.com/en-us/windows/win32/api/winnt/ns-winnt-file_notify_extended_information)。独立实测与局限见[验证报告](/D:/program/UE/GptProjectTest/plan/subplans/uai/hjson-windows-file-candidates-validation.md)。

## 5. 当前开发树Source如何消费

1. `BeginExplicitWorkspaceFileObservation`只在显式full/初始化入口建立连续观察；新目录rename发表后重新绑定真正root，再由原publisher最终验证。普通import不会因缺起点调用full。服务器停止/Editor关闭会结束会话；重开后旧目录普通import报`workspace_file_observation_origin_missing`，没有USN透明续接。
2. 成功publication的`RememberPublishedWorkspaceFileVersions`交同generation的实际文件表、未消费作者文档、公开comparison bytes与机器行spans。公开raw与AST不是同一种材料：此处不预建整个workspace AST，不保活Model。已有文件的被选版本需要树时，由明确的旧版本建立阶段构建这份**已知旧raw**，再开始Update；缺raw即停，Update失败不会整parse当前文档。
3. `SelectWorkspaceChangedFiles`只消费Windows候选＋原pending作者文档。纯目录Modified不选正文；已证明来自publication的目录创建及仍有当前字节proof的系统输出不算手改。候选不是SHA先遍历全文件产生的。
4. 真候选才读正文。原parser/typed分别判断文字范围和作者意图；0/0.0等价、合法省略、系统归一及作者消费基线仍由它们负责。文件删除/rename不是直接删除UE资产的命令。
5. 读取通知截点与实际提交/目的through独立。原source截点之后的作者候选合入原publication pending文档集合，不由旧ack退休；它不被造成本次actual。最后source/R/W字节及namespace保护仍停止真实并发冲突。
6. 同代组织/原raw/row span/安全边界缺失时，当前开发树在该接缝报具体错误，保作者bytes和原请求；已commit则保applied/pending与合法custody。不会自动整目录hash、整bucket解析、完整Model/World或重放native。

NTFS连续会话是当前接入范围；独立层的rename/目录事件能力不等于HJSON资产语义已逐一验证。当前已跑Medium字段/参数组件增删及系统回写，具体版本、耗时和仍N级职责以[当前算法说明](/D:/program/UE/GptProjectTest/plan/subplans/uai/hjson-current-algorithm-flow.md)及任务收据为准；USN/跨重启、全部领域与所有错误分支不作已验证声明。
