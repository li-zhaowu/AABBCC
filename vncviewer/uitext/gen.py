"""Writes ../UiTextTable.inc from the translations below.

Run after changing a Tr()/TrF() text in the viewer sources:
    python3 vncviewer/uitext/gen.py
It fails if a text in the sources has no translation (or a translation
changes the %-placeholders), so translations cannot silently go missing.
"""
import os
import re
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from extract import keys  # noqa: E402

# English: (Simplified Chinese, Traditional Chinese)
T = {
    # --- file transfer window ---
    '%ls - UltraVNC File Transfer': ('%ls - UltraVNC 文件传输', '%ls - UltraVNC 檔案傳輸'),
    'File Transfer - %ls': ('文件传输 - %ls', '檔案傳輸 - %ls'),
    'UltraVNC File Transfer': ('UltraVNC 文件传输', 'UltraVNC 檔案傳輸'),
    'Transfers': ('传输', '傳輸'),
    'Cancel': ('取消', '取消'),
    'Retry': ('重试', '重試'),
    'Clear finished': ('清除已完成', '清除已完成'),
    'Name': ('名称', '名稱'),
    'Progress': ('进度', '進度'),
    'Size': ('大小', '大小'),
    'Speed': ('速度', '速度'),
    'Time left': ('剩余时间', '剩餘時間'),
    'Status': ('状态', '狀態'),
    'This computer': ('本机', '本機'),
    'Remote: %ls': ('远程：%ls', '遠端：%ls'),
    'Type a folder and press Enter': ('输入文件夹路径后按回车', '輸入資料夾路徑後按 Enter'),
    'Upload  →': ('上传  →', '上傳  →'),
    '←  Download': ('←  下载', '←  下載'),
    'Type': ('类型', '類型'),
    'Modified': ('修改日期', '修改日期'),
    'Up one level (Backspace)': ('上一级 (Backspace)', '上一層 (Backspace)'),
    'Refresh (F5)': ('刷新 (F5)', '重新整理 (F5)'),
    'New folder (F7)': ('新建文件夹 (F7)', '新增資料夾 (F7)'),
    'Delete (Del)': ('删除 (Del)', '刪除 (Del)'),
    'Copy the selected items to the remote computer': ('把选中的项目复制到远程电脑', '把選取的項目複製到遠端電腦'),
    'Copy the selected items to this computer': ('把选中的项目复制到本机', '把選取的項目複製到本機'),
    'Cannot open %ls\n\n%ls': ('无法打开 %ls\n\n%ls', '無法開啟 %ls\n\n%ls'),
    'The remote files cannot be shown.\n\n%ls': ('无法显示远程文件。\n\n%ls', '無法顯示遠端檔案。\n\n%ls'),
    'Network drive': ('网络驱动器', '網路磁碟機'),
    'Removable disk': ('可移动磁盘', '卸除式磁碟'),
    'CD/DVD drive': ('CD/DVD 驱动器', 'CD/DVD 光碟機'),
    'Local disk': ('本地磁盘', '本機磁碟'),
    'Desktop': ('桌面', '桌面'),
    'Documents': ('文档', '文件'),
    'Downloads': ('下载', '下載'),
    'Home': ('用户目录', '使用者目錄'),
    'Users': ('所有用户', '所有使用者'),
    'Cannot create the folder': ('无法创建文件夹', '無法建立資料夾'),
    'Cannot rename': ('无法重命名', '無法重新命名'),
    'Cannot delete': ('无法删除', '無法刪除'),
    'Some items could not be deleted:\n\n%ls': ('有些项目无法删除：\n\n%ls', '有些項目無法刪除：\n\n%ls'),
    'Files are still being transferred.\n\nStop the transfers and close?':
        ('还有文件正在传输。\n\n要停止传输并关闭吗？', '還有檔案正在傳輸。\n\n要停止傳輸並關閉嗎？'),
    ', %u files': ('，%u 个文件', '，%u 個檔案'),
    'Waiting': ('等待中', '等待中'),
    'Scanning…': ('正在扫描…', '正在掃描…'),
    'Transferring': ('正在传输', '正在傳輸'),
    'Done': ('完成', '完成'),
    '%u skipped': ('跳过 %u 个', '略過 %u 個'),
    '%u already there': ('%u 个已存在', '%u 個已存在'),
    'Interrupted — continues when the connection is back': ('已中断 — 连接恢复后自动继续', '已中斷 — 連線恢復後自動繼續'),
    'Interrupted: %ls (Retry continues where it stopped)': ('已中断：%ls（点「重试」从断点继续）', '已中斷：%ls（按「重試」從中斷處繼續）'),
    '%u failed: %ls': ('%u 个失败：%ls', '%u 個失敗：%ls'),
    'Failed: %ls': ('失败：%ls', '失敗：%ls'),
    'Cancelled': ('已取消', '已取消'),
    'Not connected': ('未连接', '未連線'),
    'Loading…': ('正在加载…', '正在載入…'),
    '%u folders, %u files': ('%u 个文件夹，%u 个文件', '%u 個資料夾，%u 個檔案'),
    '%u selected (%ls)': ('已选 %u 项（%ls）', '已選 %u 項（%ls）'),
    '%ls free': ('可用 %ls', '可用 %ls'),
    'Delete the folder “%ls” and everything in it?': ('删除文件夹“%ls”及其中的所有内容？', '刪除資料夾「%ls」及其中的所有內容？'),
    'Delete “%ls”?': ('删除“%ls”？', '刪除「%ls」？'),
    'Delete these %u items?': ('删除这 %u 项？', '刪除這 %u 項？'),
    '\n\nThey are deleted on the remote computer and cannot be restored.':
        ('\n\n将在远程电脑上直接删除，无法恢复。', '\n\n將在遠端電腦上直接刪除，無法還原。'),
    '\n\nThey are moved to the recycle bin.': ('\n\n将移到回收站。', '\n\n將移到資源回收筒。'),
    'New folder': ('新建文件夹', '新資料夾'),
    'New folder (%d)': ('新建文件夹 (%d)', '新資料夾 (%d)'),
    'Cannot create the folder.\n\n%ls': ('无法创建文件夹。\n\n%ls', '無法建立資料夾。\n\n%ls'),
    'A name cannot be empty, end with a dot or space, or contain any of these characters:\n\n\\ / : * ? " < > |':
        ('名称不能为空，不能以点或空格结尾，也不能包含以下字符：\n\n\\ / : * ? " < > |',
         '名稱不能為空，不能以點或空格結尾，也不能包含以下字元：\n\n\\ / : * ? " < > |'),
    'Cannot rename “%ls”.\n\n%ls': ('无法重命名“%ls”。\n\n%ls', '無法重新命名「%ls」。\n\n%ls'),
    '&Open\tEnter': ('打开(&O)\tEnter', '開啟(&O)\tEnter'),
    '&Upload to the remote computer': ('上传到远程电脑(&U)', '上傳到遠端電腦(&U)'),
    '&Download to this computer': ('下载到本机(&D)', '下載到本機(&D)'),
    'Re&name\tF2': ('重命名(&N)\tF2', '重新命名(&N)\tF2'),
    '&Delete\tDel': ('删除(&D)\tDel', '刪除(&D)\tDel'),
    'New &folder\tF7': ('新建文件夹(&F)\tF7', '新增資料夾(&F)\tF7'),
    '&Refresh\tF5': ('刷新(&R)\tF5', '重新整理(&R)\tF5'),
    'Copy &path': ('复制路径(&P)', '複製路徑(&P)'),
    '%u active': ('%u 项进行中', '%u 項進行中'),
    '%ls of %ls': ('%ls / %ls', '%ls / %ls'),
    'All transfers finished': ('全部传输已结束', '全部傳輸已結束'),
    '“%ls” already exists': ('“%ls”已经存在', '「%ls」已經存在'),
    'On the remote computer, in %ls': ('在远程电脑的 %ls 中', '在遠端電腦的 %ls 中'),
    'In %ls': ('在 %ls 中', '在 %ls 中'),
    'Copying:': ('要复制的：', '要複製的：'),
    'Existing:': ('已存在的：', '已存在的：'),
    '  (older)': ('  （较旧）', '  （較舊）'),
    '  (newer)': ('  （较新）', '  （較新）'),
    '&Replace the file': ('替换文件(&R)', '取代檔案(&R)'),
    '&Skip this file': ('跳过这个文件(&S)', '略過這個檔案(&S)'),
    '&Keep both (rename the new one)': ('两个都保留（新文件改名）(&K)', '兩個都保留（新檔案改名）(&K)'),
    'Do this for &all remaining conflicts': ('对剩下的冲突都这样处理(&A)', '對剩下的衝突都這樣處理(&A)'),
    # file engine status texts (fileman::StatusName)
    'not found': ('找不到', '找不到'),
    'access denied': ('拒绝访问', '拒絕存取'),
    'already exists': ('已存在', '已存在'),
    'invalid path': ('路径无效', '路徑無效'),
    'read/write error': ('读写错误', '讀寫錯誤'),
    'too many open files': ('打开的文件太多', '開啟的檔案太多'),
    'invalid handle': ('句柄无效', '控制代碼無效'),
    'disk full': ('磁盘已满', '磁碟已滿'),
    'not supported': ('不支持', '不支援'),
    'bad request': ('请求无效', '要求無效'),
    'disconnected': ('连接已断开', '連線已中斷'),
    'cancelled': ('已取消', '已取消'),
    'no answer from the server': ('服务器没有响应', '伺服器沒有回應'),
    'error': ('错误', '錯誤'),
    # file-only mode
    'The file transfer window could not be opened.': ('无法打开文件传输窗口。', '無法開啟檔案傳輸視窗。'),
    'The server does not allow file transfer for this connection.\n\nFile transfer may be turned off in the server settings, or this connection may not be allowed to use keyboard and mouse.':
        ('服务端不允许这个连接传输文件。\n\n可能是服务端设置里关闭了文件传输，或者这个连接没有键盘鼠标权限。',
         '伺服端不允許這個連線傳輸檔案。\n\n可能是伺服端設定裡關閉了檔案傳輸，或者這個連線沒有鍵盤滑鼠權限。'),
    'UltraVNC Viewer - File transfer': ('UltraVNC 查看器 - 文件传输', 'UltraVNC 檢視器 - 檔案傳輸'),
    'This server does not support file transfer.': ('这个服务端不支持文件传输。', '這個伺服端不支援檔案傳輸。'),
    # video mode
    'The server does not offer video mode.\n\nVideo mode needs the new UltraVNC server on a machine with an NVIDIA GPU and a current NVIDIA driver (see the server log for details).':
        ('服务端不提供视频模式。\n\n视频模式需要新版 UltraVNC 服务端，并且那台机器要有 NVIDIA 显卡和较新的 NVIDIA 驱动（详情见服务端日志）。',
         '伺服端不提供視訊模式。\n\n視訊模式需要新版 UltraVNC 伺服端，並且那台機器要有 NVIDIA 顯示卡和較新的 NVIDIA 驅動程式（詳情見伺服端記錄）。'),
    'Video mode needs the "Full colors" (32 bit) pixel format.\n\nChange it in the connection options and try again.':
        ('视频模式需要“全彩色”（32 位）像素格式。\n\n请在连接选项里修改后重试。', '視訊模式需要「全彩」（32 位元）像素格式。\n\n請在連線選項裡修改後重試。'),
    'Video mode does not work with server side scaling.\n\nSet server scaling to 1/1 and try again.':
        ('视频模式不能和服务端缩放一起使用。\n\n请把服务端缩放设为 1/1 后重试。', '視訊模式不能和伺服端縮放一起使用。\n\n請把伺服端縮放設為 1/1 後重試。'),
    'UltraVNC Viewer - Video mode': ('UltraVNC 查看器 - 视频模式', 'UltraVNC 檢視器 - 視訊模式'),
    'Video mode ended:\n\n': ('视频模式已结束：\n\n', '視訊模式已結束：\n\n'),
    '(no reason given)': ('（没有给出原因）', '（沒有提供原因）'),
    # single window
    '(minimized)': ('（已最小化）', '（已最小化）'),
    'Show only one remote window': ('只显示一个远程窗口', '只顯示一個遠端視窗'),
    'Show': ('显示', '顯示'),
    'The server does not offer single window mode (it needs the new UltraVNC server).':
        ('服务端不提供单窗口模式（需要新版 UltraVNC 服务端）。', '伺服端不提供單一視窗模式（需要新版 UltraVNC 伺服端）。'),
    'Single window mode needs the "Full colors" (32 bit) pixel format.\n\nChange it in the connection options and try again.':
        ('单窗口模式需要“全彩色”（32 位）像素格式。\n\n请在连接选项里修改后重试。', '單一視窗模式需要「全彩」（32 位元）像素格式。\n\n請在連線選項裡修改後重試。'),
    'UltraVNC Viewer - Single window': ('UltraVNC 查看器 - 单窗口', 'UltraVNC 檢視器 - 單一視窗'),
    'No windows to show on the remote computer.': ('远程电脑上没有可以显示的窗口。', '遠端電腦上沒有可以顯示的視窗。'),
    'The window was minimized on the remote computer.\n\nRestore it?': ('这个窗口在远程电脑上已最小化。\n\n要还原它吗？', '這個視窗在遠端電腦上已最小化。\n\n要還原它嗎？'),
    'Single window mode ended': ('单窗口模式已结束', '單一視窗模式已結束'),
    # remote audio
    'The server does not offer remote audio.\n\nRemote audio needs the new UltraVNC server.':
        ('服务端不提供远程声音。\n\n远程声音需要新版 UltraVNC 服务端。', '伺服端不提供遠端聲音。\n\n遠端聲音需要新版 UltraVNC 伺服端。'),
    'UltraVNC Viewer - Remote audio': ('UltraVNC 查看器 - 远程声音', 'UltraVNC 檢視器 - 遠端聲音'),
    'Remote audio ended:\n\n': ('远程声音已结束：\n\n', '遠端聲音已結束：\n\n'),
    # system menu
    'Video Mode (GPU)': ('视频模式（显卡）', '視訊模式（顯示卡）'),
    'Single Window...': ('单窗口...', '單一視窗...'),
    'Remote Audio': ('远程声音', '遠端聲音'),
    'Media Settings...': ('媒体设置...', '媒體設定...'),
    # media settings dialog
    'Media Settings': ('媒体设置', '媒體設定'),
    '%d fps': ('%d 帧/秒', '%d 幀/秒'),
    'Automatic (best the GPUs support)': ('自动（两边显卡支持的最好格式）', '自動（兩邊顯示卡支援的最佳格式）'),
    '%d kbps': ('%d kbps', '%d kbps'),
    'The maximum bitrate must be between 1 and 200 Mbps.': ('最大码率必须在 1 到 200 Mbps 之间。', '最大位元率必須在 1 到 200 Mbps 之間。'),
    'Video mode (GPU)': ('视频模式（显卡）', '視訊模式（顯示卡）'),
    'Maximum bitrate (Mbps):': ('最大码率 (Mbps)：', '最大位元率 (Mbps)：'),
    'Frame rate:': ('帧率：', '影格速率：'),
    'Codec:': ('编码格式：', '編碼格式：'),
    'Remote audio': ('远程声音', '遠端聲音'),
    'Bitrate:': ('码率：', '位元率：'),
    "Mute the remote computer's speakers while listening": ('听远程声音时把远程电脑静音', '聆聽遠端聲音時將遠端電腦靜音'),
    'Turn video mode and remote audio back on after a reconnect': ('重新连接后自动恢复视频模式和远程声音', '重新連線後自動恢復視訊模式和遠端聲音'),
    'Changes apply the next time video mode or remote audio is turned on.':
        ('修改在下次打开视频模式或远程声音时生效。', '修改在下次開啟視訊模式或遠端聲音時生效。'),
    'OK': ('确定', '確定'),
}

# Texts that are not in Tr() calls in the sources but translated at run time.
EXTRA = ['not found', 'access denied', 'already exists', 'invalid path', 'read/write error', 'too many open files',
         'invalid handle', 'disk full', 'not supported', 'bad request', 'disconnected', 'cancelled',
         'no answer from the server', 'error', 'Users', 'Desktop', 'Documents', 'Downloads', 'Home']


def placeholders(s):
    return re.findall(r'%(?:ls|[du])', s)


def c_literal(s):
    out = []
    for ch in s:
        o = ord(ch)
        if ch == '\\':
            out.append('\\\\')
        elif ch == '"':
            out.append('\\"')
        elif ch == '\n':
            out.append('\\n')
        elif ch == '\t':
            out.append('\\t')
        elif 32 <= o < 127:
            out.append(ch)
        else:
            out.append('\\u%04X' % o)  # \u takes exactly 4 digits (\x would eat the next ones)
    return 'L"' + ''.join(out) + '"'


def main():
    here = os.path.dirname(os.path.abspath(__file__))
    needed = []
    for k in keys(os.path.dirname(here)) + EXTRA:
        if k not in needed:
            needed.append(k)
    missing = [k for k in needed if k not in T]
    unused = [k for k in T if k not in needed]
    bad = [k for k, (zh, tw) in T.items() if placeholders(zh) != placeholders(k) or placeholders(tw) != placeholders(k)]
    for k in missing:
        print('missing translation:', repr(k))
    for k in unused:
        print('unused translation:', repr(k))
    for k in bad:
        print('placeholders differ:', repr(k))
    if missing or bad:
        sys.exit(1)
    lines = ['// Generated by uitext/gen.py - do not edit; edit the table in gen.py.',
             '// { English, Simplified Chinese, Traditional Chinese }']
    for k in needed:
        zh, tw = T[k]
        lines.append('{ %s, %s, %s },' % (c_literal(k), c_literal(zh), c_literal(tw)))
    path = os.path.join(os.path.dirname(here), 'UiTextTable.inc')
    with open(path, 'w', newline='\r\n', encoding='ascii') as f:
        f.write('\n'.join(lines) + '\n')
    print('wrote %d texts to %s' % (len(needed), path))


if __name__ == '__main__':
    main()
