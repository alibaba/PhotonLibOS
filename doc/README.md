# Website

This website is built using [Docusaurus 2](https://docusaurus.io/), a modern static website generator.

### Installation

```
$ brew install node yarn

# Configure npm mirror/registry in advance
$ cd doc/
$ npm install
```

### Local Development

```
$ yarn start
```

This command starts a local development server and opens up a browser window. Most changes are reflected live without having to restart the server.

### Build

```
$ yarn build
```

This command generates static content into the `build` directory and can be served using any static contents hosting service.

### Deployment

Using SSH:

```
$ USE_SSH=true yarn deploy
```

Not using SSH:

```
$ GIT_USER=photonlibos yarn deploy
```

Will push to the `main` branch and use the GitHub pages for hosting

## 翻译

### 翻译HTML或者Javascript上的元素

```
$ yarn docusaurus write-translations -l cn

$ vim i18n/cn/code.json
```

### 翻译文档或者博客等Markdown文件

https://docusaurus.io/docs/i18n/tutorial#translate-markdown-files