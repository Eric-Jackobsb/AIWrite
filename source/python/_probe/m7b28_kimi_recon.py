"""M8-14 / `M7B-28` 同款侦察 —— **Kimi（kimi-web）：上传入口 + 视觉性质**（一次性探针）。

为什么这样写（**零逻辑分叉**）：
  `m7b28_doubao_recon.py` 已经把「B0 登录判定 / B1 只读入口侦察 / B2 注入 + CDP 网络回执 /
  B3 发送问答」全部实现并跑通过（物证见 `out/m7b28_doubao_recon*.json`）。本文件**只覆盖站点常量**
  后委托它的 `main()` —— 复制 979 行逻辑只会带来「两处要一起改」的风险（`R1` 的教训）。

本文件做三件事：
  1) 覆盖站点常量（站点 id / 目标 URL / Cookie 域 / 输出文件名 / 附件固件名）
  2) 鉴权 Cookie 白名单**留空**（Kimi 的鉴权名尚未实测）→ 登录判定**只按 DOM 正证据**判定，
     并把侦察到的 Cookie **名清单**写进 JSON（供下一轮把白名单补实）
  3) 输出文件名带时间戳 → **不覆盖**豆包的既有物证

用法（**需要你手动登录一次**；不代填账号密码、不绕过风控，`VB2-37`）：
    cd source/python/_probe
    python m7b28_kimi_recon.py --login     # 开有头窗口 → 你手动登录 → 优雅退出（保登录态）
    python m7b28_kimi_recon.py --recon     # 只读侦察：入口形态（file_input/drop_zone/paste_only/none）+ 选择器候选
    python m7b28_kimi_recon.py --inject    # 注入 1 张图（**不发送**）+ CDP 网络回执取证
    python m7b28_kimi_recon.py --send      # （默认关；`I18`：无注入证据不得发送）

产物：`out/m7b28_kimi_recon.b1-<时间戳>.json`

⚠️ 拿到结论后请回填两处（**不必改 C++ 代码**）：
   * `source/assets/providers.json` 的 `kimi-web` 条目：`web.attach_selector`（+ 可选 `web.upload_accept`）
   * `docs/网页版协议实测记录.md` §10：Kimi 侦察记录（入口形态 / 选择器 / 网络特征）
"""

from __future__ import annotations

import pathlib
import time

import m7b28_doubao_recon as recon  # 复用全部机制（只改常量，不改逻辑）

# --------------------------------------------------------------------- 站点常量 --
recon.SITE_ID = 'kimi-web'
recon.SITE_URL = 'https://www.kimi.com/'          # Kimi 网页版（kimi.com）
recon.SITE_COOKIE_DOMAIN = '.kimi.com'            # `D-30`：登录 Cookie 只认站点自己的域
recon.SITE_WINDOW_ARGS = ('--window-size=1440,1000',)  # 默认视口太小时附件工具条不渲染（豆包实测教训）
recon.LOGIN_STABLE_POLLS = 2

# Kimi 的鉴权 Cookie 名**尚未实测** ⇒ 留空并降级为「DOM 正证据」判定：
#   * 不猜名字（猜错会把登录判定带偏 —— 豆包 B0 误报的教训）
#   * B1 会把侦察到的 Cookie **名清单**写进 JSON，下一轮再把白名单补实
recon.AUTH_COOKIE_NAMES = frozenset()
recon.AUTH_COOKIE_MIN = 1

# 输出与固件：文件名独立 + 带时间戳 ⇒ 与豆包物证互不覆盖
recon.EVIDENCE = recon.common.OUT / ('m7b28_kimi_recon.b1-%s.json' % time.strftime('%Y%m%d-%H%M%S'))
recon.FIXTURE_DIR = recon.common.OUT / 'drive_files'
recon.IMAGE_FIXTURE = pathlib.Path(recon.FIXTURE_DIR) / 'm7b28-kimi-256x256.png'


if __name__ == '__main__':
    raise SystemExit(recon.main())
