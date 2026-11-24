# 简体中文汉化说明

本仓库是 [tarcode-apps/AIMPRemoteControl](https://github.com/tarcode-apps/AIMPRemoteControl) 的个人 fork，只为给插件加简体中文界面。

- **不是官方版本**，与 AIMP 和插件作者无关。
- 插件是 MIT 协议，原作者署名和 `LICENSE.txt` 原样保留。
- `main` 分支与官方一致，未做任何修改；**所有汉化内容都在 `zh-cn` 分支**。
- 基于官方 `plugin-v2.0.1`（提交 `8d057f2`），在 AIMP 5.40.2731（Windows x64）上实测可用。

## 改了什么

| 部分 | 文件 | 说明 |
|---|---|---|
| 网页界面 | `web-client/app/_i18n/zh.json` | 新增，312 条文案，与 `en.json` 的键和占位符一一对应 |
| 网页界面 | `web-client/app/_i18n/resources.ts` | 注册 `zh`，一行改动 |
| 插件设置页和报错 | `zh-cn/plugin-lng/english.zh.src.lng` | 译文源文件（68 条） |
| 构建脚本 | `zh-cn/plugin-lng/build-lng.py` | 把译文转成插件要的 UTF-16LE 格式并校验键 |

没有改动插件的 dll 和 C++ 源码。

## 网页界面

插件自带 i18next，按浏览器语言自动选择。中文系统的手机或浏览器打开控制页即为中文，无需设置。

```bash
cd web-client
npm ci
npm run build          # 产物在 out/
```

关闭 AIMP，用 `out/` 整个替换插件目录下的 `wwwroot`，再启动 AIMP。

## 插件设置页和报错文字

插件的 dll 里只登记了英语和俄语两种语言，简体中文界面下会回落到英语。但语言文件是运行时从 `Langs/` 目录读取的，所以把译文以 `english.lng` 的文件名放进去即可生效，不需要重新编译：

```bash
uv run python -I zh-cn/plugin-lng/build-lng.py \
    plugin/Langs/english.lng \
    zh-cn/plugin-lng/english.zh.src.lng \
    english.lng
```

关闭 AIMP，把生成的 `english.lng` 覆盖到插件目录的 `Langs/english.lng`（建议先备份原文件），再启动 AIMP。

## 已知限制

- **插件升级会覆盖以上两处，需要重新做一遍。** 新版如果新增了界面文案，`zh.json` 里没有对应键的部分会显示英文。
- AIMP 里插件设置对话框的中文没有逐项核对过，只确认了网页里的报错文字已生效。
- 32 位版 AIMP 未测试。
- 安装插件时注意：x64 的 dll 必须放在插件文件夹根部，`x64/` 子目录里的那份不会被加载。

## 同步官方更新

```bash
git fetch upstream
git checkout zh-cn
git merge upstream/main
```

冲突通常会出现在 `web-client/app/_i18n/resources.ts`，保留三个语言即可。

## 反馈

汉化问题请在本仓库提 issue，不要提到官方仓库。
