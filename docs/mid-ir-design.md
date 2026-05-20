由于之前写过一遍，这次我没有打算采用渐进式重构开发，而是一口吃成大胖子，用非常暴力的手段一口气直接搭好 AST。然后，一口气搭好 ir。

我确实这么做了，这种体验怎么说呢，其实不太友好（，要想的东西太多了，由于不是像之前那样渐进式开发，某些功能是一口气直接从 0 到完整的形态。有点漏洞在所难免对吧。hh，事实上短路求值那部分就出现了很多漏洞，不过我觉得这是引入了浮点类型的问题（？）

Anyway，就这样吧。按照我的想法，我打算从 high ir -> mid ir -> backend-riscv/arm。在 high ir 设计时汲取了 mlir （其实主要是结构化 ir）的思想，保留了完整的结构化信息。但是和 mlir 终究不一样。比如 mlir 中，专门提到了：

> Source location tracking and traceability The provenance of an operation—including its original location and applied transformations should be easily traceable within the system. This intends to address the lack-of-transparency problem, common to complex compilation systems, where it is virtually impossible to understand how the final representation was constructed from the original one.
> This is particularly problematic when compiling safety-critical and sensitive applications, where tracing lowering and optimization steps is an essential component of software certification procedures [43]. When operating on secure code such as cryptographic protocols or algorithms operating on privacy-sensitive data, the compiler often faces seemingly redundant or cumbersome computations that embed a security or privacy property not fully captured by the functional semantics of the source program: this code may prevent the exposure of side channels or harden the code against cyber or fault attacks. Optimizations may alter or completely invalidate such protections [56]; this lack of transparency is known as WYSINWYX [6] in secure compilation. One indirect goal of accurately propagating high-level information to the lower levels is to help support secure and traceable compilation.

虽然我也想这么做，但是这是个比赛嘛，呃呃，主要还是太枯燥了，不想做这些工作（也许可以让 AI 帮我写完）。可以看到我虽然在 ast 引入了 source location，但是基本上没怎么使用，而且在 ast -> ir 阶段，就 drop 掉了，并没有传播（这样 AI 就得改变我 ir 的结构体，好麻烦..要不不传播算了）。对于 op 的处理也分开了 op， func，module 的语义，我们不需要像 mlir 那样的拓展性对吧。

我也没有设计 mlir 那样的 block 参数，不过嘛，simple is the best~只要功能上没差，我觉得放松一些设计也没什么大问题。可惜的是，我这个 ir 打印出来，其实挺难看的。都怪我想着那个 : -> ，可是 -> 很好看啊（

接下来是关于 mid ir 的设计，我觉得使用 koopa ir 挺好的，或许稍微变动一下？我们的 high ir 保留了非常多的信息，mid ir 设计成什么样都是可以的，甚至可以转换到 llvm ir，但这样要做的工作量很大，也没什么必要。emm，那这样的话，最好还是只拆掉 region，改成 bb，其余保持原样即可。好像还忘了，getptr 需要改一下，或者当作 pass 处理掉？关于数组能做的优化也有很多，再仔细想想。。

唉，这是我的问题，我没有在一开始就想好 high ir 和 mid ir 的结构，现在如果要 flatten 到 mid ir 就需要修改 high ir 的结构。。重构代码好头疼的。。

差不多用了很小的改动解决这个问题，仅仅给 op 增加了后继表示可能存在 jump branch 语句，其他的最大幅度复用 high ir 架构。但是我觉得应该可以独立出来 high ir 和 mid ir 的共有部分，解除 mid ir 对 high ir 的依赖，可以做到「语义明确」，可惜如果我一开始就这么想就好了，现在不是很想改这个东西。。

现在觉得 mlir 为了可拓展性和并行之类的特性写了很多代码，他们很好，但是对于我这个小东西来说还是太大了
