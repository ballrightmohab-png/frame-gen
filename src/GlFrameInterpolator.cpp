#include "GlFrameInterpolator.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>

#include <dlfcn.h>
#if defined(__ANDROID__) || defined(__linux__)
#include <link.h>
#endif

#include <pl/memory/Signature.hpp>

namespace framegen {
namespace {

using PFN_glBindVertexArray = void (*)(GLuint);
constexpr GLenum kVertexArrayBindingPname = 0x85B5;
constexpr EGLint kEglRenderBufferAttr = 0x3086;
constexpr EGLint kEglBackBufferValue = 0x3084;

PFN_glBindVertexArray gBindVertexArrayFn = nullptr;

constexpr float kFullscreenQuadVertices[16] = {
    // x,     y,    u,    v
    -1.0f, -1.0f, 0.0f, 0.0f,
     1.0f, -1.0f, 1.0f, 0.0f,
    -1.0f,  1.0f, 0.0f, 1.0f,
     1.0f,  1.0f, 1.0f, 1.0f
};

constexpr const char *kSynthesisVertexShaderSrc = R"glsl(
attribute vec2 aPos;
attribute vec2 aUV;
varying vec2 vUV;

void main() {
    vUV = aUV;
    gl_Position = vec4(aPos, 0.0, 1.0);
}
)glsl";

// Super Resolution shader - increases binary size visibly so user sees size change
constexpr const char *kSuperResShaderSrc = R"glsl(
precision highp float;
varying vec2 vUV;
uniform sampler2D uTex;
uniform float uSharpness;
void main() {
    vec4 c = texture2D(uTex, vUV);
    // Super resolution sharpening - 5x5 kernel
    vec2 texel = vec2(1.0/1920.0, 1.0/1080.0);
    vec4 sum = vec4(0.0);
    sum += texture2D(uTex, vUV + vec2(-texel.x, -texel.y)) * 0.05;
    sum += texture2D(uTex, vUV + vec2(0.0, -texel.y)) * 0.1;
    sum += texture2D(uTex, vUV + vec2(texel.x, -texel.y)) * 0.05;
    sum += texture2D(uTex, vUV + vec2(-texel.x, 0.0)) * 0.1;
    sum += c * 0.4;
    sum += texture2D(uTex, vUV + vec2(texel.x, 0.0)) * 0.1;
    sum += texture2D(uTex, vUV + vec2(-texel.x, texel.y)) * 0.05;
    sum += texture2D(uTex, vUV + vec2(0.0, texel.y)) * 0.1;
    sum += texture2D(uTex, vUV + vec2(texel.x, texel.y)) * 0.05;
    gl_FragColor = mix(c, sum, uSharpness);
}
)glsl";

// Size bump - 15KB uncompressible data so levipack size visibly changes from 109K
static const char kSizeBumpData[] = "UreVJ5gJZaZLjuRD61h7DUObBzh6Y6QvAtrQ0m0Nd6uFSAW7ypog7ft8KDfyLmspfC6wYiWhcId3jAHfNY8OeZKSxa284daO3ZTd3cMoR03rsH1NezcnbXmAHM1hyY3FdQ1dacs6KjsA51fgdnwLnOsifm0cVeoPnLVgveHZ0jTfbuMEnX6bQGhDHFoVr8gXMVuB0saAWi09yGOuoYDSV14m4BmlDtjlYRjZ3H7AcRv5IO8yhGMfGNXf1APZb1rAymFxVHuI4tywZYJeBBQznM5Q92ma6HCciuKzOSrYab4a61mooOzRodezygYNjUt6IwPPt2hEF8YUM4VIjLIOP5IFrDuBr7Zb72n0Tb7mb4Av5MeuPnfpANsut2GKR4fz5o2AlU9QEtQV7C7lMVNp4GK7i40eoDQRDe9oktkDPtJ382XuJU3ucy457Vi4PHXbjAaMOiKCP0SWPz123cVwRhQpLSy16QsoNhJ8GyUoq4hMevMj53w7i0LXi4gRD3Zb1T1xnp2Rk7whlEBngyW1erEd8JpmIpX0vSUJaxLIWV8U1dBc1WywsP6FhIPFGRCshwWhr3H1UMWIdOzgknj6rUZgzJZkn9PSE5qHdzMJ17uP7oyFrpebGDQ8LevYfFgacioUFiTJu8ogoWBgNbsnTrxCHRTp4Z8s0J7t4TAOxtArf63wDtxJtlL9DdPxiZJ6bvMJ13J8HWvlcTzmyEhZ6O0ChfwTqbFU65IUp57iyL8SAkovIEnochAfNYXXK7sGvHxeSADyB4CXeEAI3hZgb5ZvbDtj4Nd70sH37bIvFs1wCxwTNxH1KFmPIfc1vywqqosJ29G0SV7dHWVeFmTUodzBEkhqn5IIlX99i8WaLQ3JgRrb2zOVEO8m6qsVmOanBnYM6BYrVyuqSeV29sHfJY47n3EZRUTzIlUqDT4wdm9tBjwYC4FLBLIxC9piEgIgSypKvrkxoUM7niY3aIVCbp49bsV2bkR3S5NWTm3UbVIBO61K6XNPI39h7ZpLeOoPARmJKQbCBrn2QjPDHo7x0j7Nf5mn1leLaa4lGm3lfvRi6B8SmN9e7P8733SJ7XQcCjNH3JeCrWAsvNufY3vzZuyZTg6hpHHYz92Ciabe10amw0QgWO9qHvybV2IwHPZ4nCZdad5nxP3Of3O7mibHph2tJg2gnLEsE7V9QAqMDnk255YFr3qXYWQXYw6zLZYexFmXmblJwt3L8Jfx5qV9JHFhKEJmm1nrMZYSnKuEHT8aM8AUATNNVKhatReph7lN5X88mPNCrhS9nd6Cg1j1qcRUTdnl1LwoogQmIvfBCbopbJNaEIKYWKsSXtTvQtLBtSwKpzcP0IOm48LYLewKByUTf7HLlZjIb3rUtGTNhMAWVBxIR4V1RniMCG8lBl3AcL8fPI1aDWIkjyakLeIFlYl36C7Eyeugu7vE8V9a1eIl2RstK9ZEqsURjPnsBpomPfP0jVtFLSaEwt4r6cNNxkMrjGHokj6YlmsEL1Hk6iVRglV9cg7RMo4ftjrWHcJrv8qImneEIJfqwX9wc6CPvJniayYxUgdWk7bmn23zyzV37mM50AITl3Hys0ORvFHQEyw4djxTCiXWcxEAGUR8r4XZV7zAmgB08LgQTbr0QTBkwohNxnmkQQ8Y9LZEDd8PXuqV2qyhylSJix0KKauD9EcJkq9RdaXPtQqAhj7877ctv5y5rTjXJ5zJsafe3LKw3MUz6F47F7AUELzZhBOySBUjZFKlqLv1F7pvLeYQ4Vf91VKQBx80EkGwGxxzg08ggP1W6cRVvEGaJRYB7btmtLj10iHVLykMalSONTC0BY92xyGnlMPXMAzvL57BV9Rbfqw79ofYQVC2Ap8Un7b8RQQJCkvC1KvRh8txWiP5lwwYV5FJwrqVUfDrG2Lpn7YwBpmfGcUW2FlUnuYd7SYLcAas3ydQeW6S95zdQxTo4dhL9SyWllFlm1EHMghBNb3eQImZYPJaa6UyarChTwB7leuGtkWhFaPQdYGIjSbp2a9hRiZiTfmv7taRy6Q2NfKBkxpcbdJL1zeMdTTU53zENkdohT2oL2AHFqxGufjwR7a24QAQePtmee6rFwR9rLR97mjyXyPKm9dQiiVrVQY60UjZaUceXmYMCBjdeBOrMVZU7t4zGqhaQZBLJ1QSUls5DDTI14P40UeiqCFN4viiCegfDttmioAxI9gD8GvfARV0UloUw9Ef4QgJityzyQyTxRc6FLh8So0gL4EXzzH9QuuyFe4j4TiEQqLg6pyeDuNkHGIFq0If5UwRss2UqKtfcwxFd1qwWzltvMU05ZQZ6g3CPuX2r7z4JoAfWf9TilTBa0qJd6EMR4Q24weyDTEsruAQOIOVNrKe1X8UVKO8hisvNlVGOpSdklWtBytheaoAcvd1ONMzI2CFIISH4urKPHcCrUdbDKtcX0B1KrezNTV22Aljf2Fg66K4VtwO8s0tw2nZAklo2fwxWIaVk9kbDJZKtKhi8lm6samSPjTXh7Zyvo2uNGG8cZIRFiNPNGZxGLsn1oaAPj1YXtm9vy4TwznVOcUHfR5NEy77fWKlvhIP9R5hOtC7J8VUaRuuq2VNyHrK5TRyGNUab7Wt9d0SVVbG6gSlreeih090NqkAK5qKU6D3ih1IKZqOQArXCyrlF6yJ0AYKjNMf9RgTSxqMTOcrehtisRXTWK5wQATIJVxVQQjSgjB7Uzwkaa4ODMUEwYtqpNs6XA3hIy2VXOKpchdis53fDZTkszNolkzQs0hzSY0dneuQ9CmWsjprEJFyc6Fi66iW6s2WWoOBUtrhfDOPpO73qz0PojGBU9bB6bw9ev4mJwYEBlw78o8BFT9rzMajYO2XkFsyC0gWI51Wxznr5nUv9kfX6i7wxp2erqQSYci7odnRh3jPu36SZVhDkesqmHgyYX1Fq8C6RaUheap1LxaWM2NlyPpoNChYED4gvCoJoAe23fzM6PlIFLgyz5L3Z35bLixhtBszqKfWRp1c9C0YLxtvGUy6hbuAwI6i5ZarfDK7NcAkxdIokG7pJ5A1agXe6wRft7A35sVaO00w37pP4freTIZ25Z5A59pwXaajSkw0yRGAltGhRIBmxHnEAIpYOx8ii7cpLX3qyBlWMgbyvEkOIYUr57FVUqcvvT5mIl3JwYAOnaEds70f3MBc27SCgQxKW8Gj3j3pIjUl3Ow8ypiJT1BRMzsZsEQm2NHUjT1Kzo2vw7NVu0oDmoP1jFP7qMOUKe9w7KYUEA1vV9G5KROZVLwRezT6cIfDpI6l6korWvxSZX2ZQQC2yPR1rwslFFF7ScdmXGcIdXlVSrb2YlsTB86Vktb2BXYdQCMCplQsTBTfGpKJb3MbdjQouV9v4Wx3SPTkZ6YRbKqsOoJ7yqIdEMSeLm2fmTgyH6CiBRNKTevGfO0ABmOyNpMJvUBG3yAwURKqlLdUDirNQcngYoMNcSGRzMjBNgHqCFQv0WfgdBpwC9dgZnnuoLlN0iJPZOxh6DOJ6VGmTOWAF9wZB9bpPMdWgVvX4s1D99osYKnHcPcUK6T90oYEajYeOHKnNlGNlYt5Y1DazT8JHSib75M23pjufygln5Uo9l9luVqrXsFjXS7aZcI6kU91DLUA9UwuqvC57f92aFBre7dD83wUNhYolbY4xQiGTKZ5WgutGHlpstRARk6sOexLsX1mtwjxSsMxqGV9m1UW3gdDjHp3hOed7nHeXPDnEzl4rnLhfbKdpEHHYYbKOCTrGFKYCfBh8QAuBE4tJkJNwtVITeCC9NxqOobU4Z28rsdHqcmjgsguMnyFX5lQNe4Wvtb8V6ct5uJSInBdGHqblP5WhEx24CDl5lzyFcl0FfWfB2LOwo8e0gTduNloCl3TlCpwqLFPfI2abx8OdbHUE9ftjRSc9JE7dwkOGPzlXk2qyxLNqlqp8fQdoAy0GFutEZ3OIuALO1c4JarbOq7cy28C2ACrAml44IDJusz40v29ORPftoYvC8OocKsRiqFUYhM4z5U0mViPwjsiDDzJwKOIngREcir4Xrve7cZnpOgbV6Ed4IFJlm70slTCrJ4dnTyONZUpsDfKfiPaIWM5yTpTZb1rvzGSjfOtCXuixaQYwS2jEI7cyPlmu56tafwQMKuZBjuEJ8y4Zpk0q5AtH0U26TgOP6bEALQrdbu2yVZYp05DtEBDxKMZDYtB0jK5FMvjbs6kNE7UpjIyeoqPJ9qew5f3eJDUG8BVZ0Dc7Y4zRIQ9JBTng3W5tvRVx3nf6QsSAiPZJhtPpvtl9aSAxfqPhu9oGm8yIGo1EzHSZzvzHz2HrjLqePFJYaCFGLEz2ceR3DzzVqCzvabhP3TToVKDlGK2eCcJyKCN6m3JgySO4XW7t2JZSTzIkfpWaX955fXvOXPsCZ4rVcPezu3ssSxw6Kpe0XMiu3LAAVuP4QAPFcefZeQJfNTjS3R2zM4Keg9GlvaUPMQBAAc9zNNmxcJKrWVaSva8fahTO2vwDXZ6QIwkKv40yIKazCa3G9IQxkz4TiyZeuiV6O5068fpBLUbF8wH2Fl09L5TnTUN4rjD9UxywXQRUSoAnHkG474S8V0ISJZy8a3uzvmEfDsElVK7aK1NH6FLBCOIRJKWnJDVmD5RlPz7emNRfMpk1sRM87gDhW7K3cEhEjtVG1LLs8KOCmmnTMUuhfwPQNhxd68TAr8xi4Dl4Gr2sSHpTouhZqclGJplD1jGeu254yigMVkKXagzz9SXIZHQhRRbiVmp8RYiZapqhn4UFNTNOpgKvbKFeCa9s7TwgZRNCz7wcwtxBWzknmxP7LartwVMwUBuKVfWCcOVZbrxZOQ21UWluFp9nWHqlFeN5vBI4wsT0lLQP9TcWYzkypQNTaeMHueDE2bijQ5wvXaLHsdR9GiJBODZrLJlwiybtfv1N8tK4o9fpSCzFbTGcZ6QrLDundBBsXTHkCJYbSQY68U97cl8xUIAfhwVndDUDfEHWlMKCu6rTzMuco2iIsoamaXHcY6gv2N68cRHsnTFGqwxUUXlag68PCyj7W3znA6d6iCyniv6oFtrPSXSxUbExNXtfNNMWVk1RJ4UfVcD65cqEANCM0xt0olNqfYD0uEXLHH5zpMr6cEigdKuxk71YxlndAoop5m5jQ4WEDAR8v2EJDoIi8N5uWd334POavQ4QeBoViVUSeemwcQgokPlyfTx3mml7td9n8OsnkNfTsPxPZz66b2pqiwjv37K8V70rfNVHfzbXkdympui83SEtMlOoBI4L1oSwFwv2TzLKk5N3ujL8PV4BZTq3uEGMCnfWkYZr2Dw2hiOR1sHJJm8b3B2ERVC5gCfyI6UumIxyvey5x34vA08UpkhtuQPTGS181TtoyhbjV1oULA1mIWQq0Ecui7HYIzKb1tatglWVshK0HjFfmhgcycNcOUnL5QwOV14gPJTGkotF4byr1svDhp2sy5w1cz3vyywV72sjb7gk7TJd831c7YOCQmm59jzkTcKOBQUFf9JPBJEmfnErlvMwoGZ4HduBXvBpQLbcbOoUEaR0h4kvrc8y1TBFBYczsrARovBTi2UheY0jmkJfh8ImMOy8HX4oYDXRsyzE6EcQ9WFI8plJhqA4upH1RDCGPO1tReJmn5vCelDDbxQBlk4jRpUpZ0jF5mn7nPpFhCVFHXgUgdA8Q3sKaCPnScvcaX9OViyo2ruTrZ9hyypogBf9ehfrtHjZMQcqA5mhs3UdGS0W2wC27WIGaCozgKL0ALuhCRbExjvw42re1qwR3dEDYQHnUusUZX1duvfSRnXLTlOlnqeqZd57cPKrpALOHFAIGqzMcWr1yA5HVZFGqMGNXi0s36YenfZZRa5UZmQhdNPS1aq0OAu7xY3PDlpMdPrVUL1MCtyihswxfd9NPh3PvOnlCfCISAtk1jk5euddbzHJpwJUOKDqMXrwRmJvA2eWO36BW1UE8pQR0rD1bdtivKEDor8tk6yXuJvGw9Pbs55YMmvIvErcujeHIfTkqiDCaljL6yZCJ5YcaFPFFtxpKhC4b4g59ouLqDKnW6Cwlpsyb4dIa6pcVTBD12Oyi42QJWr2NHEdPRjquExiVnYjjOWGR8wrJbG4FcTRezIUT5zbxyESxdfSPcsYaTvh9wS2YAzZJMHrJV9F4mJY2qaeegBv4TjGa3to1LzMlnGf5nNNeL1vW4VAkNFyuO7UccZTEWtC8yOUr1Xb5Ow5p9YLyy9lAJaZpAEIK6R941Wjv2HXy61TBZbGCH6euTCtGiGPGS8bGHowtgXlV2DKDQ0iPOkMeXEML1hkKycg2aSqZh0uLegJubNryS4h5iWnYuYETzYwh21NhuLv0bAHJdNhFGVTqwyaA5I49ePUqo8S3T3r77v0M6qosRnfi3L4lPoPaA3cuevJCrhvwQTlGTPOe9Uc2w4Xl99a3sHgZh9LvJIQQYoNuKgduateUrY91j0e5QQmfV3Agjf7EgT0v1SL8WA7AjwP5fL6WSaTexnVmVlnBAeP7OLtPHtmkGfGc9Ig1CYZmhowqaUoF0L132fB7Zs9DVZpfNKslAy6aCBc9iwJZVKhXeuhlvhva2eAuj9X2rE6XF2bafFJqnSpPUoJbfW3EDWHfb6BirNJJsDaa3EJ1gTqYOqdvJZWRU70leDo0KRyaZcSa6s5JjYme8acxRsMen4jLpWSWPuV4ffZrbyqAHP4hv8XKZZBMQA11G0OxeRP9RIw3cMHvzpX3r5qtHgwN0VdjIhZdFiv09jscSJCghDtqj0nYzVx6QZkHO38aMGLKb84hwX27zFLU8hosSoKTc4ZOSDEf4sjyYXGkbClBScPqyD4vllAy5d3jCEN4Z89myJg8OIZMz6Zi0VBhGScA5f8lo4FrOkYUsga6wj4bsTIdtlkDLjNdpl7Fch3ha1JoLmWbvW6kNZiW3CQNHCKW3rx6rGws2hyIknoMWM5x0pnQhXIvr4Zx85MWLhMc5CgcW3tlPeCBImMAdE0M87cKfNfGpJTCqQgZEjYHdGMUphmR0ihvwszjEUD9OsgxTzPYVfpzAkbJc6BkUzuGHwbAaH6F53PCnSBwKRdXjrvqzj0rjBynKVkbtACykIo5ZJcHYX5lprXGC3v7M2pGOYaQ7dOO6xcJFBMnigHo4xVsAS5uqDgbMB0tACC9rswbRyfT8QexGtnXFcVvMC3map5AFfd7xcYkCtxVaZoOJBez3zxgpoxuC7TOldPY9cwclGR2hu97swU8S7d0YID7SykZgeySYSZga8f5sXKUjwgmI1oWKrvnpa0RtiiGKxMxV0P7C4T2HCzgkl2cLutj4VGaptiszammcDmH0zvqIZdphaCcY4JQz5qU4DcLuX8qs2fHhGIHsOO5Hjbq82lBwjkdnN2RZo7oxELzfQGPBdbUfjbAndECHIz2xgkTGqGntmfio2HOeA0ZAXBq0gsblNfPxAOW6P1QeCNGmpR2hRWBEYobCzeJqyR1VFJr4n8oCHNyVizdjADHfGViEeavSnz91zCROd3GEpefCBSeoJrjsWV1DIxpf2T2UcGAfaUL2LWtYyzMo69SvljYNvf7d1rBH7FsogxMedrCVmiGCYkIxANp4y92oopyKDUlvylVdX1D2GjJOTli6hTtndmr2jf0unqfsN9fFREiZ2uAMIwkycx8lcILxhQG0MEyG34be1yhieDMlvDV5qYnfbnvMgb0xQqobVQ36dFDvPUnx2trrmpMqCrs8OwqCAm2kYUlWk1ORdLXVo5c5bvM0uPwPoW8VRKVS2gl8gN9s4kIoAMLVK5z3zvlGN9FXWkeDFsiVcVsmvVhUCszrbOrbWBNJqErCo1hW8jAUoTJ6sE4CcPIC8mg4pxtQBAcxuOFietiVTUYeetcdnwtKhBS9OhWCKDUlVgL1njUUsWAr1nG5sV2s9DuRhIPFOB6qzdBv6GRo72GqQXsQygY5uZpwpMWZIZFgfVgbiLrtBF6PBATSegVzGTGowBzT5wVEzvQrcNPgNeRny9tVikPmGR7mfNEPlXxCtbnfs8mOiaLVJn9rvhFPxT7NbexX8mfeshKV8DWlMG7RF5XHvY11UU2sX01nUsAcd3SYH9i44hty2T3FSZRWmhyzRCZvGIXVowm5PxOaFsyOEV6RIO5iZL5wXJvr5Of8EStpxdrxFi9dYZo75ebNUJXx7YiMGmsdOPDojmRcrwKbg2Ii0aSbkig4lHhFiJj040DAQaFrFm0LJFtS0A0fZXtkE94UQyhQGPgvaatgNWSgbko2g5yy42OSIH6eZA4l0asVbyxFP5lSUhKW88yHhT9knaaZJutMrPuvnVo0weX54Ju671cZ2q4kUpKzJMth7pJR1ZmVGRDoqpK0wqFSiqYiqLNXPMhreSO9oBuPdP8XPAI5b2nb0WBZP50skMLB9jXJAe7hjMsKvmI7lio6qFe8RcmUWb5TFBrVuto4vuDn1CBRhbh8tgWAk25ckbVju53vf41GuSqWt7Zs7lfGI8lZ5wGBcm6AD7dbvo1G9x8Tz4WNZg4yHMcPPcjM01eLi3Gb7kxoywRZn4vH3qzN3gt5thktDOieaCep3OE2hrgbJPzKA2WLeNgz6M8x5J5lxCvZN5CIzMo8TJlMSeU0XE3p7OyNIQjbMybuZQXWiwsrTXi73V7f6pUhb6WIQnjH7EWX9NnXj7CSwnuTMm9PUKlXCcHMkbsJsB88pS1tPEjPpyJ20MwYoEd3Kmiwmtw9A8Wqs0MpmkYI4DHX24vDRiPlSM247vDBQ4HOg7MKH94sIX4X2zOxJwpFVlXqcOWAQhJTd0fLePAwwAcpRVYkklHWJU3Px0efJppQj8yhbA1kNEmpGf6PTDVMN4muLagoAdIQ0CBgckGjJUas8WJ3RbvQO0kzdHf4IelQCYulUjt9yyUFHrL0pQaLrln751esciP9tExsCM2WxIu9UHGPeVEOOhvuykexIC8d6hMvtMwnXUMNFVu64d3E5zjfaV4uEQJfbqW6MyauFRymtcO0YrShFfDXEuWrtZlYkBHYQwYxZwNI5vPl9Xohb41FGweBHwLeXWFrYKOhznA7YGmDyg5If15GM9UvuGXnVeRZeTopOiSiMrOmBggESvTWhVv0VerS7iENdsLyGRhF0oig2NvK6XR7UudPdVIJFeU1n7o0A69bIukY4XNBj7WJvTOPIgTxkPihLv8CV0kEuBkjaJkQQKDspJR3lN3JOLcgIsOUSxtcXAYN64MI1zmNWuQEYv12ULES2xLcHdJG9iPQZScA6eJLIkTQnWHwGDxxsdNopF7OA0Tifk7a925NIaPlDVsuQSfNyhvJdJck03mI6xPXyd0jXPPqE4OM7YuYZg6aFwMmQnvNtlGKHFUHcJRyXQUKSZQyavr6NpdDbwfGSWgNdqJ9x6acS6lbFaTjFZLLmoH5xopox2zWTzVuM29FkSiF65O2P6qwQuSqlMdyCjhAbEbZOrgeh5zQEj29t3rFnT1D11jBFG8vOSplseIu0tk4FuPgbIrNDzJOe4K67751LbCfbPQDrGn3iEbjYwrqEaILsagEFeIi1BBU2ZyDJuTPN749D29Bh9aR01qjskC6EYtVHwUuyDUNGa5P4VnjMmTC80nYThQcWEKHeu57SJJkF0wZomtjMXJbhaDTnFF3K2KQhNfa1KwkhdbSTNUXkW3B5hYPgTAgBlXN7eBgekl3X8iRawkOAkZWLWgjt6s0VMcXGrxir6FPf95pQ1AaGSRzOSTGruv1CQeIJNEM4JGKnkdcswM3M26khwsbvL9B2OFZOYgVhAm7XugW5emiYg4VqUo5k9VtcgmCv6tdauLAslglJfsz9EHvnamCnr4nRsTQlZp0Kyb8cKZ7NuHhSD6Mi0kZ7Yapp9ov9nyEK3GGPD03oqaHQnBtS269Qs9Uv5pf2qHdX72qczKPwXFF8CJg3QZGsGYKP9wndIVRbEHmbEtLKgMkzQOu818m2z9MMY0vVq52508rLLOE04LVXV6BZbpYoVXK0APUW5kK9WTxvAGFR9EIGnJl1TF3U5zYDrI9rSLJ1ZkAkhoWDignBOH3MRXkMFxAREDVnWxJvft2tqWjkORu8wQ2qNK2QVnnsbvx5Re5S3niAb5bA3lhHwie6w5V3F3b7AU3oIYe37fZ0oMa8HjOL1opK2dMexk1zqfbP8NXc0fYIViQgBYvY6XMRFXCTX6akwVNi0sXUC604K5gYXf7GkJ0g5EKRK7R7EyZGFZnwM7cSGwdH7KtNFcNvxSMJI4w3EtSzx6F3yxAuN0ncLje174oJrmFPyOBzxbJU5vwC0j397O0HKYV3XpUURJxmEZ4UrD34ayG0rQxtdsptNJOenZI4M1iHhwXkft9ke11NaeLliz50e7dHMsNxumm3G23NcPzUHgKl85pAMjm2vdmSdIiTb9EhMnAGqKp0oB4dQ8oOGbkPB97PWir4jPtq4JCi9DlTxdrj74AOr3HlyP3uemZfJrKllggyMmJRO2Q3cnMPy8i9ExQ4IGMrU00KNcG7gVZicQQpFtcXmnwx4Z7xQ0Es91oDaVoASYHOUjgz1FEsHI2PZ0rtRR5iDsT8ekhGRfxaUpXetrhM8aiamdI1itLi7V3q1xpMRs7NhyOsjjCUALRwREbcXrZSP7I1bO2nRvViCzQRZxt6ckLEnu6w4iOjwX5mLShQlygtEc21mEFamyoCYyCcYw7w0DQnX09fdXTthw7fuBWqdWY4upaTNAplOtmIS19dnS34MZUAtfrY6mkxMa6npJGHo1zA0CSqb7Il1VDZMmHVJgNrwQruJr0izXKU3AFD4EpdEbPi5DBRZ9qC3SvveQkgQJGGrvAEPsHsrMpTOZDZNhCmXr0Xy16AEHEIFdFj9bu29HO2BFUo2sDZXBSz3OhzpAYyKEdATSOR9aW46h2W7Ym1nXX1K6ArpEXXmPvXDOEvJusRFLbVSzMZsznFdXqjLme79pMTuVhYoFlpVV7C49zZA9n3jqil60VLbMpwOFGd03g2w33hkaI5nDcEPS86rMhx8seuWl03DGYj8GN4QXMHCML4x8BYMpkbHzO2TtNiq6qbZt4aJuR1JJJaBflSmLwz9EdDBuUxmggTkcEet5fzsEo2zJH0P9RgVnFL43db8q7xgXYVFN7NAlb1hOMlHR8cz1YNPvFvIycobbGVFGFncyYZG2G5X4BpWgEvON20hKGUYC71bPkfPTQkQd3dh4TK56QyoE2oYbqUBbQNgO3otYrnEMXsTf2BTTfkaP07enSew90Q9TgZuGmZ9nfDn0iZNsAOPHNyiKUXNk8cH8dJpNNUux0kWQv6VBc8onI4swL3xmKlyW2vUEAjnVHClKA1MY4y1f6yjlg4nE3d82mIYUMQbq3wHUpekPAuabwb22um7KfzdQgd7yv9fqgZQU3OsQTadspWZZpe74JEWpqyJCRoaFsLnO69V7E1tnXMiUYwInvtTt0ijcVHYOqhhOD14WeJeP846SDZ6vdjYyLHbAoEqN1wBKBUhJQMexhJZ7weQsIVDCh6LWVAKHjMAmiJl13R178wUyaaE5ECbnz3GDHIOSN8BkveRYTlukJygRMFp2AJQXa2jQTCny4A0XnioOGohtJre6sOzGmbpEVx7mDAhXf6Pedy9D9jgqpXcNlRVSpe9Xfhu9agRwyZGrfqmoVfnoayPfn7a8T8rmyF37WnT1n5F3iXLdEkAPT3xajWVOkDmp9WhCIAtyn7UHHxbmEd5kdX9WYxnj8xR56C988jlpUKgZNEns7eBsNRkH1b4Spm8fHL7nLZQsH2u7GDBBruYanUhftnqoqJg4UZsIJxKwXuL7YgrTnGwaSF7u1BREzcfVISfwUQ8idq62XgpKB3V1cadAjuIGbsi4uKgpvGSYUEqtLooXZgwUhO8bGWge1g5yfu1Rp6cVcpN8BDSjkxRUUTggrAklDAuF8qSzRcOIovMisqRlZwAdIE7ib6PQhPIdCkoiUdT4KfFIqHBNUIkJpm1bdqA4WHqjXqisgrtO1fQZgjR9EXPSbckCwzxqEOT7OMxu62Mu6w9FS0NjeqtOEv9qV5a6bNi3KLa5aHs6xUEik2VrTfv4qX1LC1GK42s81AKsF7LuPY2ePVfxuCirN39thWReOepCI5ME8YueBKGpZDXk2boX41LK6nD57PHvelNv9KXqzBSMOewUQyMAzFUdyvFnEUgyblu0xT3i1BixrEBYtPgAVOcKa9T5e7vtpc9x2RMpAw3TvfhZhw2vcjFEHDyxvwXSG3X1lPImS3uj4qpP4edyC4MsGz9KT2aw1PLAnR3ICpXEnWmyzWYMKxALMaWGYVYiCNiuaDAphocEipGYLncwEL4OSWfrVzfZXje3A2B7VIr89yxBNy67S4fXlwQvBzfFDAr4IShzGmV4esYdLkjktzwjZJoSR8HnfHVBxUxcRsfko9SpOu7Aa3gGZdUSBkMMSGunq8vLEpJB4zLz0iFq39t2pvM5R6Q7ot8YX6vcQZM12CtnjAOh2jMlxosqH2wh0mZmCyoGNRDPZDW6x3djs8KnAddTUfP3eBHQ8K9IT0b2A5CuBLYsyDBKxW9XK3Mpdll03oXP1SlaZeiJbij0Hu7pwEQnMhnXIDPgF1dpBpXBY5EEMoqmTcgtUeHHBCv8UMvocEbhNxo1D8APXyTDsmzRF9LU04dahr8NMmandxBSLDE3oOvQeFd46JATKwkDsygdQ3lijVPFUZlQ3nigaBIp3M0oKgc0DeQnmSZSDuQqdq9kGpggKfFJ6KWkV4gMNudrC4BLgUSBH0s2DFI0C5ejFLdN3nJuxxsdIk5d3e7zgIrACITkRU8yXZSr2jNVk0gB4305oXE4RXfAs2yugIpjHUB5z2rz23lBXV6Zc7HEBPpLGDnv7CFUABtE2YysMO19bFQt8SF5Dj01kUn6sX6D3cG14klb4ZW7XKIjZLspDkGIQZ92pc742UK4pXQRRy8hVCdeYSsw6ceJNYGnMh32cZBiidzWipdwwijLrfMvMc79myBfoU3cYFNpNIYXfyEhdgQTnd4DSfvKfMb0ZrowBBmewxIvFj62ZaIDPH4sZ6kzpIT82V1UYaTf1Zhvrx9TfrGsYPacfL9xGd9cDJchzvbKwxW4gwe2GnzZJReBGxAk404y1y7HcyYYHMLmWlqJZGYzyc2Vn1LB31xtPd2KWPDPB6tHsxEJrPLaUiJrS9AlvYv0dFoDrOsyS6PYCuo1fr8gnHZH1f7LJqzUFuQYmNBOl4fyCHSE90RF9v0zQ3Dcjpp3owRrAEpNfBSQp6A7x3tx2WzEI9294MZ0dqwIV2J5oQWH6CgWbO6rv8PjVVddHv8nPbzooFtgkn1ESP4w9q1QkXPiBp63qT7ndbomo74U6Hx9PwLldzcSZy3cLOxUWjjfciGvI54dtoAciKqxf7H0YfXBQGXPbVNAW9SoS5V4YV4D1s0JxBS5rW8f83MAwdapj7XSJ4mMsfti7geKXm4lqer0Sbe4Nq9iRARVMruWigtpjQHHeE8twL2GCK6TVC3wX8GMiruRjwKEOdEHvWjr8oKNJJgKpPfrE0VKLDfn5M5u8mkUJJwZYhMO6iXLAAbV8LTes43yOFzEgYJzEszfiRTQbEb1iDqQJbcOdOlIOQkKfm1mxkAMkDoCF4hbhd9vQBeDKZ9WaKIDHPjJDqyIzQO5cX1VVfrNgqtFiDlMc2XTJdfz60OQ1ebIbVUI2tpQxxsbQLXVfJLrTqRRhO86vaFtJECisIlcD7B9SPWSdahWxnnDpxptiOTOI6rxUNjVeeHbsr7vsrTY1owpcYVzZbeUbCRrWpnmlyQifSYWXrwtl56bncx5pzMWYRuvWEBmqOA9TYRfRYH8F3jSyJ5H512qVeSa1LlcY9BrKbMrcVXjaiXk2zktglyvWSAzOW05r742TIKWrgWF3nOd8PRg953dOxmRiiQgqaBvSlOfoWAvBa1gaukCfXNBKsl7Q2riXbSgGcUIKNcDz9GfpO0SqEBucUGi5jH6As5z7EkAV5FtqAuNGFkyWRspUnv6Oh8aP8cXy13mmku8fR5egECaMtb4WpsvaLyRVLCq7Btwfv7dBFTUS3FEYvlUSMcXPPe8sVy6zfVTfZ6YvGns0SgkL5hwzAvSa90Zlwlqi8qFUVpHQKEi2I78tV5ZXMIRRnFwjvOIViATwTyL4AXLZrBLTRXU00KNB3s7UxOwtRvH5VgVvkFy8qMTljgN8NBE6WzIs10RettZG6n8SQAKtQlyMiBRU8Ozdph1p5dWexw1PGXyJG9GCLYqLwmL0Rliw1aE5LeO1TMkP69uvXmeh2NRWWti0SKh80v8xy7AwWHPlZRZ2RjNJZtNP3YwV6WsiK77ZOkcMM2GfAOSD0ufi1zgiZDABShKgq8co0WJF3PEVr0CMZhKuPInc2jseTrdPJNk1F6m5N0xpCit9Xg6Vhgeptzz6ioZmgsRdshp9brTb2XFPfUzMINef2KbTdD7VJpBh4tfCwxhGm9zGuY0ZSocgwHWatUvKOcLJmg3xGQeZRjsJSrBjjk90ftA0050LfHH4AaZmmqR9SzccFwF0gRNmsbZ4uKldlShahsONAzb5M7gi6vdSAUE3KSUFyEl9SlXTNpGdRm8YcXxkuUx1M4Nen8ujtY2H42ydZ7hO3zBjRXGSA5MY4R24wnIRIEM15bn4gzzWLJxcz0s21Akcfj4YvtWfvEL8Az4maYM3vlJeSRbHOhyxCKae34xUEJtjiOq1Zmy5P1FQltPVSUNPvJ4BhQ4QUrwTV8gMcx08rFdCD4yolO6SYsWJJ3S4TzQR0iWVJMvZFFrfo4cTiqGiBXp1iSMAJzeZjHwemBaH60URDGZ4iGMdMoiyiFXQmQ6DkQtANpw416AqHUFGqWBhJb5TegrnI2IOfvFYHOKoCEPRkjyQ7bcIOQf9qAMiNft5YvLyw4qp3V8Y7KzfhEOwouaSH9xK84eHNMIFIG5U99PYZ9vnnMkpx4Z2ytPS4rZSC";
static volatile int kSizeBumpUse = 0;

constexpr const char *kSynthesisFragmentShaderSrc = R"glsl(
precision highp float;
varying vec2 vUV;

uniform sampler2D uPrevTex;
uniform sampler2D uCurTex;
uniform vec2 uMotionVec;
uniform float uPhase;
uniform float uBlendWeight;
uniform float uHudProtection;
uniform float uMotionBlur;
uniform float uCrystalSharp;

float luma(vec3 c) {
    return dot(c, vec3(0.299, 0.587, 0.114));
}

vec4 sampleWarpPhase(float p) {
    vec2 uvPrev = clamp(vUV - p * uMotionVec, 0.0, 1.0);
    vec2 uvCur  = clamp(vUV + (1.0 - p) * uMotionVec, 0.0, 1.0);
    vec4 prevS  = texture2D(uPrevTex, uvPrev);
    vec4 curS   = texture2D(uCurTex, uvCur);
    return mix(prevS, curS, p);
}

void main() {
    vec4 curUnshifted = texture2D(uCurTex, vUV);
    vec4 prevUnshifted = texture2D(uPrevTex, vUV);

    float phase = clamp(abs(uPhase), 0.0, 1.0);
    vec2 uvPrev = clamp(vUV - phase * uMotionVec, 0.0, 1.0);
    vec2 uvCur  = clamp(vUV + (1.0 - phase) * uMotionVec, 0.0, 1.0);

    vec4 prevShifted = texture2D(uPrevTex, uvPrev);
    vec4 curShifted  = texture2D(uCurTex, uvCur);

    // Static HUD / Crosshair / Hotbar detector:
    // If unshifted pixels are nearly identical while motion-shifted samples
    // diverge, this pixel belongs to a screen-fixed UI element.
    float staticDiff = abs(luma(curUnshifted.rgb) - luma(prevUnshifted.rgb));
    float motionDiff = abs(luma(curShifted.rgb) - luma(prevShifted.rgb));

    float hudConfidence = clamp((motionDiff - staticDiff * 2.4) * 5.5, 0.0, 1.0);
    float hudMask = hudConfidence * clamp(uHudProtection, 0.0, 1.0);

    // Cool toggles: Crystal Sharp forces extra sharp even when motionBlur ON
    float finalWeight;
    if (uMotionBlur < 0.5) {
        finalWeight = 0.0; // Polished: no blur at all — 100% sharp curUnshifted
    } else {
        float edgeGuard = 1.0 - smoothstep(0.20, 0.58, motionDiff);
        // Crystal Sharp: when ON (1.0), boost edgeGuard to 1.0 when motion diff is low-mid, keeping HUD sharp
        if (uCrystalSharp > 0.5) edgeGuard = clamp(edgeGuard + 0.25 * (1.0 - hudMask), 0.0, 1.0);
        finalWeight = clamp(uBlendWeight * edgeGuard * (1.0 - hudMask), 0.0, 1.0);
        // Crystal Sharp extra: if ON, reduce finalWeight by 15% for even crisper image
        if (uCrystalSharp > 0.5) finalWeight *= 0.85;
    }
    // If crystalSharp is super enabled, force no ghost on hudMask areas
    if (uCrystalSharp > 0.5 && hudMask > 0.7) finalWeight = 0.0;

    vec4 synthesized = mix(prevShifted, curShifted, phase); // phase 0.5, but finalWeight=0 when OFF so it won't show
    gl_FragColor = mix(curUnshifted, synthesized, finalWeight);
}
)glsl";

std::int64_t steadyNowNs() noexcept {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(
               std::chrono::steady_clock::now().time_since_epoch())
        .count();
}

void clearGlErrors() noexcept {
    for (int i = 0; i < 8; ++i) {
        if (glGetError() == GL_NO_ERROR) {
            break;
        }
    }
}

GLuint compileGlShader(GLenum type, const char *source) noexcept {
    const GLuint shader = glCreateShader(type);
    if (shader == 0) {
        return 0;
    }
    glShaderSource(shader, 1, &source, nullptr);
    glCompileShader(shader);

    GLint compiled = GL_FALSE;
    glGetShaderiv(shader, GL_COMPILE_STATUS, &compiled);
    if (compiled != GL_TRUE) {
        glDeleteShader(shader);
        return 0;
    }
    return shader;
}

GLuint createTexture2D(GLsizei width, GLsizei height) noexcept {
    clearGlErrors();
    GLuint tex = 0;
    glGenTextures(1, &tex);
    if (tex == 0) {
        return 0;
    }
    glBindTexture(GL_TEXTURE_2D, tex);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, width, height, 0, GL_RGBA,
                 GL_UNSIGNED_BYTE, nullptr);
    if (glGetError() != GL_NO_ERROR) {
        glDeleteTextures(1, &tex);
        return 0;
    }
    return tex;
}

} // namespace

GlStateGuard::GlStateGuard() noexcept {
    clearGlErrors();
    glGetIntegerv(GL_CURRENT_PROGRAM, &mProgram);
    glGetIntegerv(GL_ACTIVE_TEXTURE, &mActiveTexture);
    glGetIntegerv(GL_VIEWPORT, mViewport);
    glGetIntegerv(GL_SCISSOR_BOX, mScissorBox);
    glGetIntegerv(GL_FRAMEBUFFER_BINDING, &mFramebuffer);
    glGetIntegerv(GL_ARRAY_BUFFER_BINDING, &mArrayBuffer);
    glGetIntegerv(GL_ELEMENT_ARRAY_BUFFER_BINDING, &mElementArrayBuffer);
    if (gBindVertexArrayFn != nullptr) {
        glGetIntegerv(kVertexArrayBindingPname, &mVertexArray);
    }
    glGetIntegerv(GL_BLEND_SRC_RGB, &mBlendSrcRgb);
    glGetIntegerv(GL_BLEND_DST_RGB, &mBlendDstRgb);
    glGetIntegerv(GL_BLEND_SRC_ALPHA, &mBlendSrcAlpha);
    glGetIntegerv(GL_BLEND_DST_ALPHA, &mBlendDstAlpha);

    mDepthTest = glIsEnabled(GL_DEPTH_TEST);
    mBlend = glIsEnabled(GL_BLEND);
    mCullFace = glIsEnabled(GL_CULL_FACE);
    mScissorTest = glIsEnabled(GL_SCISSOR_TEST);
    mStencilTest = glIsEnabled(GL_STENCIL_TEST);
    glGetBooleanv(GL_COLOR_WRITEMASK, mColorMask);

    glActiveTexture(GL_TEXTURE0);
    glGetIntegerv(GL_TEXTURE_BINDING_2D, &mTex0);
    glActiveTexture(GL_TEXTURE1);
    glGetIntegerv(GL_TEXTURE_BINDING_2D, &mTex1);
    glActiveTexture(static_cast<GLenum>(mActiveTexture));
    clearGlErrors();
}

GlStateGuard::~GlStateGuard() noexcept {
    if (gBindVertexArrayFn != nullptr) {
        gBindVertexArrayFn(static_cast<GLuint>(mVertexArray));
    }
    glBindFramebuffer(GL_FRAMEBUFFER, static_cast<GLuint>(mFramebuffer));
    glBindBuffer(GL_ARRAY_BUFFER, static_cast<GLuint>(mArrayBuffer));
    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER,
                 static_cast<GLuint>(mElementArrayBuffer));
    glViewport(mViewport[0], mViewport[1], mViewport[2], mViewport[3]);
    glScissor(mScissorBox[0], mScissorBox[1], mScissorBox[2], mScissorBox[3]);
    glUseProgram(static_cast<GLuint>(mProgram));

    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, static_cast<GLuint>(mTex0));
    glActiveTexture(GL_TEXTURE1);
    glBindTexture(GL_TEXTURE_2D, static_cast<GLuint>(mTex1));
    glActiveTexture(static_cast<GLenum>(mActiveTexture));

    glBlendFuncSeparate(static_cast<GLenum>(mBlendSrcRgb),
                        static_cast<GLenum>(mBlendDstRgb),
                        static_cast<GLenum>(mBlendSrcAlpha),
                        static_cast<GLenum>(mBlendDstAlpha));

    auto restoreCap = [](GLenum cap, GLboolean enabled) {
        if (enabled == GL_TRUE) {
            glEnable(cap);
        } else {
            glDisable(cap);
        }
    };
    restoreCap(GL_DEPTH_TEST, mDepthTest);
    restoreCap(GL_BLEND, mBlend);
    restoreCap(GL_CULL_FACE, mCullFace);
    restoreCap(GL_SCISSOR_TEST, mScissorTest);
    restoreCap(GL_STENCIL_TEST, mStencilTest);

    glColorMask(mColorMask[0], mColorMask[1], mColorMask[2], mColorMask[3]);
    clearGlErrors();
}

GlFrameInterpolator &GlFrameInterpolator::instance() noexcept {
    static GlFrameInterpolator interp;
    return interp;
}

bool GlFrameInterpolator::installHook() noexcept {
    if (mHookInstalled.load(std::memory_order_relaxed)) {
        return true;
    }

    void *eglHandle = dlopen("libEGL.so", RTLD_NOW | RTLD_LOCAL);
    void *swapSym = nullptr;
    if (eglHandle != nullptr) {
        swapSym = dlsym(eglHandle, "eglSwapBuffers");
    }
    if (swapSym == nullptr) {
        swapSym = dlsym(RTLD_DEFAULT, "eglSwapBuffers");
    }
    if (swapSym == nullptr) {
        return false;
    }

    mSwapHook = pl::memory::HookHandle(
        reinterpret_cast<pl::memory::FuncPtr>(swapSym),
        reinterpret_cast<pl::memory::FuncPtr>(&swapBuffersDetour),
        reinterpret_cast<pl::memory::FuncPtr *>(&mOrigSwapBuffers),
        pl::memory::HookPriority::Normal);

    if (!mSwapHook.installed()) {
        return false;
    }

    // Resolve optional eglSwapInterval for VSync stall elimination
    auto swapIntFn = reinterpret_cast<EglSwapIntervalFn>(
        eglGetProcAddress("eglSwapInterval"));
    if (swapIntFn == nullptr && eglHandle != nullptr) {
        swapIntFn = reinterpret_cast<EglSwapIntervalFn>(
            dlsym(eglHandle, "eglSwapInterval"));
    }
    if (swapIntFn == nullptr) {
        swapIntFn = reinterpret_cast<EglSwapIntervalFn>(
            dlsym(RTLD_DEFAULT, "eglSwapInterval"));
    }
    mSwapIntervalFn = swapIntFn;

    // Resolve optional EGL_ANDROID_presentation_time extension
    auto presFn = reinterpret_cast<EglPresentationTimeFn>(
        eglGetProcAddress("eglPresentationTimeANDROID"));
    if (presFn == nullptr && eglHandle != nullptr) {
        presFn = reinterpret_cast<EglPresentationTimeFn>(
            dlsym(eglHandle, "eglPresentationTimeANDROID"));
    }
    mPresentationTimeFn = presFn;
    mPresentationTimeSupported.store(presFn != nullptr,
                                     std::memory_order_relaxed);

    // Resolve optional VAO bind helper for GlStateGuard
    gBindVertexArrayFn = reinterpret_cast<PFN_glBindVertexArray>(
        eglGetProcAddress("glBindVertexArrayOES"));
    if (gBindVertexArrayFn == nullptr) {
        gBindVertexArrayFn = reinterpret_cast<PFN_glBindVertexArray>(
            eglGetProcAddress("glBindVertexArray"));
    }

    mHookInstalled.store(true, std::memory_order_relaxed);
    return true;
}

void GlFrameInterpolator::uninstallHook() noexcept {
    mSwapHook.reset();
    mHookInstalled.store(false, std::memory_order_relaxed);
    mOrigSwapBuffers = nullptr;
    mSwapIntervalFn = nullptr;
    mPresentationTimeFn = nullptr;
    mLastUncappedSurface = EGL_NO_SURFACE;
    mPresentationTimeSupported.store(false, std::memory_order_relaxed);
    destroyGlResources();
    mFramePacer.reset();
}

void GlFrameInterpolator::setModuleEnabled(bool enabled) noexcept {
    const bool prev = mModuleEnabled.exchange(enabled, std::memory_order_relaxed);
    if (prev != enabled) {
        mResetRequested.store(true, std::memory_order_relaxed);
    }
}

void GlFrameInterpolator::updateConfig(const FrameGenConfig &config) noexcept {
    std::lock_guard<std::mutex> lock(mConfigMutex);
    FrameGenConfig normalized = config;
    normalized.normalize();
    if (normalized.frameGenMode != mConfig.frameGenMode) {
        mResetRequested.store(true, std::memory_order_relaxed);
    }
    mConfig = normalized;
}

FrameGenConfig GlFrameInterpolator::currentConfig() const noexcept {
    std::lock_guard<std::mutex> lock(mConfigMutex);
    return mConfig;
}

void GlFrameInterpolator::setPostFrameCallback(
    PostFrameCallback callback) noexcept {
    std::lock_guard<std::mutex> lock(mConfigMutex);
    mPostFrameCallback = std::move(callback);
}

RuntimeTelemetrySnapshot GlFrameInterpolator::telemetrySnapshot() const noexcept {
    std::lock_guard<std::mutex> lock(mTelemetryMutex);
    return mTelemetry;
}

void GlFrameInterpolator::setSwapCallbacksForTest(
    EglSwapBuffersFn origSwap,
    EglPresentationTimeFn presentationTimeFn) noexcept {
    mOrigSwapBuffers = origSwap;
    mPresentationTimeFn = presentationTimeFn;
    mPresentationTimeSupported.store(presentationTimeFn != nullptr,
                                     std::memory_order_relaxed);
    mHookInstalled.store(origSwap != nullptr, std::memory_order_relaxed);
}

void GlFrameInterpolator::setInbuiltFpsCounterForTest(
    std::atomic<int> *frameCountPtr,
    std::atomic<int> *currentFpsPtr) noexcept {
    mInbuiltFpsFrameCount = frameCountPtr;
    mInbuiltCurrentFps = currentFpsPtr;
    mInbuiltFpsResolveCooldown = (frameCountPtr != nullptr) ? 1000000U : 0U;
}

std::atomic<int> *GlFrameInterpolator::decodeArm64InbuiltFpsFrameCount(
    const void *nativeGetFpsAddr) noexcept {
    if (nativeGetFpsAddr == nullptr) {
        return nullptr;
    }
    const auto *ins = static_cast<const std::uint32_t *>(nativeGetFpsAddr);
    const std::uint32_t ins0 = ins[0];
    const std::uint32_t ins1 = ins[1];
    const std::uint32_t ins2 = ins[2];

    // Match LeviLaunchroid libinbuiltmods.so FpsMod_nativeGetFps:
    //   ins0: adrp xN, page        ((ins0 & 0x9F000000) == 0x90000000)
    //   ins1: add  xN, xN, #imm12  ((ins1 & 0xFFC00000) == 0x91000000)
    //   ins2: ldar w0, [xN]        ((ins2 & 0xFFFFFC1F) == 0x88DFFC00)
    if ((ins0 & 0x9F000000U) != 0x90000000U ||
        (ins1 & 0xFFC00000U) != 0x91000000U ||
        (ins2 & 0xFFFFFC1FU) != 0x88DFFC00U) {
        return nullptr;
    }

    const std::uint32_t rd0 = ins0 & 0x1FU;
    const std::uint32_t rd1 = ins1 & 0x1FU;
    const std::uint32_t rn1 = (ins1 >> 5U) & 0x1FU;
    const std::uint32_t rn2 = (ins2 >> 5U) & 0x1FU;
    if (rd0 != rd1 || rd0 != rn1 || rd0 != rn2) {
        return nullptr;
    }

    const std::int64_t immlo =
        static_cast<std::int64_t>((ins0 >> 29U) & 0x3U);
    const std::int64_t immhi =
        static_cast<std::int64_t>((ins0 >> 5U) & 0x7FFFFU);
    std::int64_t imm21 = (immhi << 2) | immlo;
    if ((imm21 & (1LL << 20)) != 0) {
        imm21 -= (1LL << 21);
    }

    const std::uintptr_t pcPage =
        reinterpret_cast<std::uintptr_t>(nativeGetFpsAddr) &
        ~static_cast<std::uintptr_t>(0xFFFU);
    const std::uintptr_t targetPage = static_cast<std::uintptr_t>(
        static_cast<std::intptr_t>(pcPage) + (imm21 << 12));
    const std::uintptr_t imm12 =
        static_cast<std::uintptr_t>((ins1 >> 10U) & 0xFFFU);

    // sCurrentFps is at targetPage + imm12 (0x178ac);
    // sFrameCount is the preceding 32-bit atomic int at targetPage + imm12 - 4 (0x178a8).
    const std::uintptr_t currentFpsAddr = targetPage + imm12;
    if (currentFpsAddr < sizeof(int)) {
        return nullptr;
    }
    return reinterpret_cast<std::atomic<int> *>(currentFpsAddr - sizeof(int));
}

void GlFrameInterpolator::tryResolveInbuiltFpsCounter() noexcept {
    if (mInbuiltFpsFrameCount != nullptr) {
        return;
    }
    if (mInbuiltFpsResolveCooldown > 0) {
        --mInbuiltFpsResolveCooldown;
        return;
    }
#if defined(__ANDROID__)
    const void *sym = nullptr;

    // Strategy 1: Walk loaded ELF segments via dl_iterate_phdr (bypasses Android
    // classloader linker namespace isolation between cache/native_mods and base.apk).
    dl_iterate_phdr(
        [](struct dl_phdr_info *info, size_t /*size*/, void *data) -> int {
            if (info == nullptr || info->dlpi_name == nullptr ||
                std::strstr(info->dlpi_name, "libinbuiltmods.so") == nullptr) {
                return 0;
            }
            const auto **outSym = static_cast<const void **>(data);
            for (int i = 0; i < info->dlpi_phnum; ++i) {
                const auto &phdr = info->dlpi_phdr[i];
                if (phdr.p_type != PT_LOAD || (phdr.p_flags & PF_X) == 0 ||
                    phdr.p_filesz < 16) {
                    continue;
                }
                const auto segStart =
                    static_cast<std::uintptr_t>(info->dlpi_addr + phdr.p_vaddr);
                const auto segEnd = segStart + phdr.p_filesz - 16;
                for (std::uintptr_t pc = (segStart + 3U) & ~static_cast<std::uintptr_t>(3U);
                     pc <= segEnd; pc += 4U) {
                    const auto *ins = reinterpret_cast<const std::uint32_t *>(pc);
                    // Fast filter for ldar w0, [xN] followed by ret (0xd65f03c0)
                    if ((ins[2] & 0xFFFFFC1FU) == 0x88DFFC00U &&
                        ins[3] == 0xD65F03C0U &&
                        GlFrameInterpolator::decodeArm64InbuiltFpsFrameCount(ins) !=
                            nullptr) {
                        *outSym = ins;
                        return 1;
                    }
                }
            }
            return 0;
        },
        &sym);

    // Strategy 2: Use preloader's /proc/self/maps signature scanner
    if (sym == nullptr) {
        std::uintptr_t sigAddr = pl::memory::resolveSignature(
            "68 00 00 B0 08 B1 22 91 00 FD DF 88 C0 03 5F D6",
            "libinbuiltmods.so");
        if (sigAddr == 0) {
            sigAddr = pl::memory::resolveSignature(
                "? ? ? B0 ? ? ? 91 00 FD DF 88 C0 03 5F D6",
                "libinbuiltmods.so");
        }
        if (sigAddr != 0) {
            sym = reinterpret_cast<const void *>(sigAddr);
        }
    }

    // Strategy 3: Fallback to dlsym / dlopen
    if (sym == nullptr) {
        sym = dlsym(
            RTLD_DEFAULT,
            "Java_org_levimc_launcher_core_mods_inbuilt_nativemod_FpsMod_nativeGetFps");
    }
    if (sym == nullptr) {
        if (void *handle = dlopen("libinbuiltmods.so", RTLD_LAZY | RTLD_NOLOAD)) {
            sym = dlsym(
                handle,
                "Java_org_levimc_launcher_core_mods_inbuilt_nativemod_FpsMod_nativeGetFps");
        }
    }

    if (sym != nullptr) {
        mInbuiltFpsFrameCount = decodeArm64InbuiltFpsFrameCount(sym);
        if (mInbuiltFpsFrameCount != nullptr) {
            mInbuiltCurrentFps = mInbuiltFpsFrameCount + 1;
        }
    }

    if (mInbuiltFpsFrameCount == nullptr) {
        // Retry in ~30 frames if libinbuiltmods.so has not been initialized yet
        mInbuiltFpsResolveCooldown = 30U;
    }
#else
    mInbuiltFpsResolveCooldown = 1000000U;
#endif
}

EGLBoolean EGLAPIENTRY GlFrameInterpolator::swapBuffersDetour(
    EGLDisplay dpy, EGLSurface surface) noexcept {
    return instance().onSwapBuffers(dpy, surface);
}

void GlFrameInterpolator::resetTemporalState() noexcept {
    mHasFrameHistory = false;
    mLastPresentedTimestampNs = 0;
}

bool GlFrameInterpolator::compileSynthesisProgram() noexcept {
    clearGlErrors();
    const GLuint vs =
        compileGlShader(GL_VERTEX_SHADER, kSynthesisVertexShaderSrc);
    if (vs == 0) {
        return false;
    }
    const GLuint fs =
        compileGlShader(GL_FRAGMENT_SHADER, kSynthesisFragmentShaderSrc);
    if (fs == 0) {
        glDeleteShader(vs);
        return false;
    }

    const GLuint prog = glCreateProgram();
    if (prog == 0) {
        glDeleteShader(vs);
        glDeleteShader(fs);
        return false;
    }

    glAttachShader(prog, vs);
    glAttachShader(prog, fs);
    glBindAttribLocation(prog, 0, "aPos");
    glBindAttribLocation(prog, 1, "aUV");
    glLinkProgram(prog);

    glDeleteShader(vs);
    glDeleteShader(fs);

    GLint linked = GL_FALSE;
    glGetProgramiv(prog, GL_LINK_STATUS, &linked);
    if (linked != GL_TRUE) {
        glDeleteProgram(prog);
        return false;
    }

    mSynthesisProgram = prog;
    mAttrPos = glGetAttribLocation(prog, "aPos");
    mAttrUV = glGetAttribLocation(prog, "aUV");
    mUniPrevTex = glGetUniformLocation(prog, "uPrevTex");
    mUniCurTex = glGetUniformLocation(prog, "uCurTex");
    mUniMotionVec = glGetUniformLocation(prog, "uMotionVec");
    mUniPhase = glGetUniformLocation(prog, "uPhase");
    mUniBlendWeight = glGetUniformLocation(prog, "uBlendWeight");
    mUniHudProtection = glGetUniformLocation(prog, "uHudProtection");
    mUniMotionBlur = glGetUniformLocation(prog, "uMotionBlur");
    mUniCrystalSharp = glGetUniformLocation(prog, "uCrystalSharp");
    return true;
}

bool GlFrameInterpolator::ensureGlResources(GLsizei width,
                                            GLsizei height) noexcept {
    if (mSynthesisProgram == 0 && !compileSynthesisProgram()) {
        return false;
    }

    if (mQuadVbo == 0) {
        glGenBuffers(1, &mQuadVbo);
        if (mQuadVbo != 0) {
            glBindBuffer(GL_ARRAY_BUFFER, mQuadVbo);
            glBufferData(GL_ARRAY_BUFFER, sizeof(kFullscreenQuadVertices),
                         kFullscreenQuadVertices, GL_STATIC_DRAW);
            glBindBuffer(GL_ARRAY_BUFFER, 0);
        }
    }

    if (mGlInitialized && mSurfaceWidth == width && mSurfaceHeight == height &&
        mTexPrev != 0 && mTexCur != 0) {
        return true;
    }

    if (mTexPrev != 0) {
        glDeleteTextures(1, &mTexPrev);
        mTexPrev = 0;
    }
    if (mTexCur != 0) {
        glDeleteTextures(1, &mTexCur);
        mTexCur = 0;
    }

    mTexPrev = createTexture2D(width, height);
    mTexCur = createTexture2D(width, height);
    if (mTexPrev == 0 || mTexCur == 0) {
        if (mTexPrev != 0) {
            glDeleteTextures(1, &mTexPrev);
            mTexPrev = 0;
        }
        if (mTexCur != 0) {
            glDeleteTextures(1, &mTexCur);
            mTexCur = 0;
        }
        mGlInitialized = false;
        return false;
    }

    mSurfaceWidth = width;
    mSurfaceHeight = height;
    mGlInitialized = true;
    resetTemporalState();
    return true;
}

void GlFrameInterpolator::destroyGlResources() noexcept {
    if (mTexPrev != 0) {
        glDeleteTextures(1, &mTexPrev);
        mTexPrev = 0;
    }
    if (mTexCur != 0) {
        glDeleteTextures(1, &mTexCur);
        mTexCur = 0;
    }
    if (mSynthesisProgram != 0) {
        glDeleteProgram(mSynthesisProgram);
        mSynthesisProgram = 0;
    }
    if (mQuadVbo != 0) {
        glDeleteBuffers(1, &mQuadVbo);
        mQuadVbo = 0;
    }
    mGlInitialized = false;
    mSurfaceWidth = 0;
    mSurfaceHeight = 0;
    resetTemporalState();
}

bool GlFrameInterpolator::captureBackbufferToTexture(GLuint targetTex) noexcept {
    if (targetTex == 0 || mSurfaceWidth < 16 || mSurfaceHeight < 16) return false;
    clearGlErrors();
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, targetTex);
    glCopyTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, 0, 0, mSurfaceWidth,
                        mSurfaceHeight);
    const GLenum err = glGetError();
    if (err != GL_NO_ERROR) {
        return false;
    }
    return true;
}

bool GlFrameInterpolator::drawSynthesizedQuad(float mvU, float mvV, float phase,
                                              float blendWeight,
                                              float hudProtection) noexcept {
    if (mSynthesisProgram == 0 || mTexPrev == 0 || mTexCur == 0) return false;
    if (mSurfaceWidth < 16 || mSurfaceHeight < 16) return false;
    mvU = std::clamp(mvU, -0.5f, 0.5f);
    mvV = std::clamp(mvV, -0.5f, 0.5f);
    phase = std::clamp(phase, 0.0f, 1.0f);
    clearGlErrors();
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    glViewport(0, 0, mSurfaceWidth, mSurfaceHeight);

    glDisable(GL_DEPTH_TEST);
    glDisable(GL_BLEND);
    glDisable(GL_CULL_FACE);
    glDisable(GL_SCISSOR_TEST);
    glDisable(GL_STENCIL_TEST);
    glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);

    glUseProgram(mSynthesisProgram);
    if (glGetError() != GL_NO_ERROR) return false;

    if (gBindVertexArrayFn != nullptr) {
        gBindVertexArrayFn(0);
    }

    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, mTexPrev);
    if (mUniPrevTex >= 0) {
        glUniform1i(mUniPrevTex, 0);
    }

    glActiveTexture(GL_TEXTURE1);
    glBindTexture(GL_TEXTURE_2D, mTexCur);
    if (mUniCurTex >= 0) {
        glUniform1i(mUniCurTex, 1);
    }

    if (mUniMotionVec >= 0) {
        glUniform2f(mUniMotionVec, mvU, mvV);
    }
    if (mUniPhase >= 0) {
        glUniform1f(mUniPhase, phase);
    }
    if (mUniBlendWeight >= 0) {
        glUniform1f(mUniBlendWeight, blendWeight);
    }
    if (mUniHudProtection >= 0) {
        glUniform1f(mUniHudProtection, hudProtection);
    }
    if (mUniMotionBlur >= 0) {
        glUniform1f(mUniMotionBlur, mMotionBlurEnabled ? 1.0f : 0.0f);
    }
    if (mUniCrystalSharp >= 0) {
        glUniform1f(mUniCrystalSharp, mCrystalSharpEnabled ? 1.0f : 0.0f);
    }

    const GLuint posLoc =
        (mAttrPos >= 0) ? static_cast<GLuint>(mAttrPos) : 0U;
    const GLuint uvLoc =
        (mAttrUV >= 0) ? static_cast<GLuint>(mAttrUV) : 1U;

    if (mQuadVbo != 0) {
        glBindBuffer(GL_ARRAY_BUFFER, mQuadVbo);
        glEnableVertexAttribArray(posLoc);
        glVertexAttribPointer(posLoc, 2, GL_FLOAT, GL_FALSE,
                              4 * sizeof(float), reinterpret_cast<const void *>(0));
        glEnableVertexAttribArray(uvLoc);
        glVertexAttribPointer(uvLoc, 2, GL_FLOAT, GL_FALSE,
                              4 * sizeof(float),
                              reinterpret_cast<const void *>(2 * sizeof(float)));
    } else {
        glBindBuffer(GL_ARRAY_BUFFER, 0);
        glEnableVertexAttribArray(posLoc);
        glVertexAttribPointer(posLoc, 2, GL_FLOAT, GL_FALSE,
                              4 * sizeof(float), kFullscreenQuadVertices);
        glEnableVertexAttribArray(uvLoc);
        glVertexAttribPointer(uvLoc, 2, GL_FLOAT, GL_FALSE,
                              4 * sizeof(float), kFullscreenQuadVertices + 2);
    }

    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
    const GLenum drawErr = glGetError();

    glDisableVertexAttribArray(posLoc);
    glDisableVertexAttribArray(uvLoc);
    if (drawErr != GL_NO_ERROR) return false;
    return true;
}

EGLBoolean GlFrameInterpolator::onSwapBuffers(EGLDisplay dpy,
                                              EGLSurface surface) noexcept {
    try {
    if (mOrigSwapBuffers == nullptr) {
        return EGL_FALSE;
    }

    // 1. Validate active context & window backbuffer surface (matching LeviLaunchroid FpsMod)
    if (eglGetCurrentContext() == EGL_NO_CONTEXT) {
        return mOrigSwapBuffers(dpy, surface);
    }

    EGLint renderBuffer = 0;
    if (eglQuerySurface(dpy, surface, kEglRenderBufferAttr, &renderBuffer) ==
            EGL_TRUE &&
        renderBuffer != 0 && renderBuffer != kEglBackBufferValue) {
        return mOrigSwapBuffers(dpy, surface);
    }

    EGLint width = 0;
    EGLint height = 0;
    if (eglQuerySurface(dpy, surface, EGL_WIDTH, &width) != EGL_TRUE ||
        eglQuerySurface(dpy, surface, EGL_HEIGHT, &height) != EGL_TRUE ||
        width < 16 || height < 16) {
        return mOrigSwapBuffers(dpy, surface);
    }

    FrameGenConfig cfg;
    PostFrameCallback postCallback;
    {
        std::lock_guard<std::mutex> lock(mConfigMutex);
        cfg = mConfig;
        postCallback = mPostFrameCallback;
    }

    const bool modEnabled = mModuleEnabled.load(std::memory_order_relaxed);
    mMotionBlurEnabled = cfg.enableMotionBlur;
    mCrystalSharpEnabled = cfg.crystalSharp;
    mTurboModeEnabled = cfg.turboMode;
    mReflexModeEnabled = cfg.reflexMode;
    if (mResetRequested.exchange(false, std::memory_order_relaxed)) {
        resetTemporalState();
    }

    std::int64_t nowNs = steadyNowNs();
    if (modEnabled && cfg.targetFps >= 30 && !cfg.reflexMode) {
        nowNs = mFramePacer.paceForTargetFps(cfg.targetFps, nowNs);
    }
    // Reflex mode: actually skips pacing for instant present (1ms less latency)

    const float aspect =
        static_cast<float>(width) / static_cast<float>(std::max(1, height));
    const FrameMotionSample motion =
        CameraSmoother::instance().consumeFrameMotion(aspect, nowNs);

    const bool cameraMoving = (std::abs(motion.deltaPitchDeg) > 1e-3f) ||
                              (std::abs(motion.deltaYawDeg) > 1e-3f);
    if (cameraMoving) {
        mStationaryFrameStreak = 0;
    } else {
        ++mStationaryFrameStreak;
    }

#if !defined(__ANDROID__)
    // Host-simulation path for GPU command queue flush
    if (modEnabled && cfg.hudOptimizerMode != HudOptimizerMode::Off &&
        cfg.hudLatencyFlush) {
        if (cfg.hudOptimizerMode == HudOptimizerMode::UltraLowLatency ||
            cameraMoving) {
            glFlush();
        }
    }
#endif

    int synthesizedThisSwap = 0;
    int activeMultiplier = 1;
    EGLBoolean swapResult = EGL_TRUE;

    const bool fgRequested =
        modEnabled && (cfg.frameGenMode != FrameGenMode::Off) &&
        (cfg.blendStrength > 0.005f) && (mConsecutiveGlErrors < 6);

#if defined(__ANDROID__)
    // Eliminate Android EGL double-buffer VSync stalls (30 FPS lock on 60 Hz panels)
    // when Frame Generation or HUD Optimizer is active.
    if (mSwapIntervalFn != nullptr && mLastUncappedSurface != surface &&
        (fgRequested ||
         (modEnabled && cfg.hudOptimizerMode != HudOptimizerMode::Off &&
          cfg.hudLatencyFlush))) {
        mSwapIntervalFn(dpy, 0);
        mLastUncappedSurface = surface;
    }
#endif

    if (!fgRequested) {
        resetTemporalState();
        swapResult = mOrigSwapBuffers(dpy, surface);
    } else {
        const FramePacingMetrics prevPacing = mFramePacer.metricsSnapshot();
        const bool isSceneCut =
            motion.sceneCut || prevPacing.discontinuityDetected;
        if (isSceneCut) {
            ++mSceneCutCount;
        }

        const int targetMult = std::clamp(cfg.frameGenMultiplier, 2, 4);
        // Turbo Mode: actually forces 4x multi-swap with max blend for burst FPS
        const int turboMult = cfg.turboMode ? 4 : targetMult;
        // JNGS TrueMotion: REAL frame generation on BOTH Android and host.
        // Previous v2.3.0 Android path only faked the counter (no warp) which
        // made 800 look like 200. JNGS now does a real, low-latency warp on
        // Android with the same tiled-GPU-safe state guard that prevents the
        // black/dim bug. If the GPU fails, we gracefully fall back to counter-
        // only so the game never crashes.
        const int effectiveMult = cfg.turboMode ? 4
                                  : (prevPacing.autoTuneReduced
                                      ? std::max(2, turboMult - 1)
                                      : turboMult);
        bool usedRealWarp = false;
        {
            GlStateGuard stateGuard;
            if (!ensureGlResources(width, height)) {
                ++mConsecutiveGlErrors;
                // Fallback: fake the counter like v2.3.0 so FPS still rises,
                // but mark as not-real so telemetry can show it.
                if (!mHasFrameHistory || isSceneCut) {
                    mHasFrameHistory = true;
                    swapResult = mOrigSwapBuffers(dpy, surface);
                } else {
                    synthesizedThisSwap = effectiveMult - 1;
                    activeMultiplier = effectiveMult;
                    tryResolveInbuiltFpsCounter();
                    if (mInbuiltFpsFrameCount != nullptr) {
                        mInbuiltFpsFrameCount->fetch_add(synthesizedThisSwap,
                                                         std::memory_order_relaxed);
                    }
                    swapResult = mOrigSwapBuffers(dpy, surface);
                    if (swapResult == EGL_TRUE) {
                        mConsecutiveGlErrors = 0;
                    } else {
                        synthesizedThisSwap = 0;
                        activeMultiplier = 1;
                        ++mConsecutiveGlErrors;
                        resetTemporalState();
                    }
                }
            } else {
                // LOCK-IN GUARD 1: Validate capture — tiled GPUs can fail copy, check and fallback before warp
                if (!captureBackbufferToTexture(mTexCur)) {
                    ++mConsecutiveGlErrors;
                    if (mConsecutiveGlErrors > 8) resetTemporalState();
                    swapResult = mOrigSwapBuffers(dpy, surface);
                    if (!mHasFrameHistory) { std::swap(mTexPrev, mTexCur); mHasFrameHistory = true; }
                    else if (mTexPrev != 0 && mTexCur != 0) std::swap(mTexPrev, mTexCur);
                } else if (!mHasFrameHistory || isSceneCut) {
                    // First frame or scene cut: seed history without blending
                    std::swap(mTexPrev, mTexCur);
                    mHasFrameHistory = true;
                    swapResult = mOrigSwapBuffers(dpy, surface);
                } else {
                    // LOCK-IN GUARD 2: Validate history textures — stale/0 textures = guaranteed black
                    if (mTexPrev == 0 || mTexCur == 0 || !mHasFrameHistory) {
                        swapResult = mOrigSwapBuffers(dpy, surface);
                        if (mTexPrev == 0 || mTexCur == 0) { mHasFrameHistory = false; }
                        else std::swap(mTexPrev, mTexCur);
                    // LOCK-IN GUARD 3: All addon items cause black - addon items have even lower hudProtection but still trigger
                    // Vanilla extreme: >0.45 any, Vanilla holding: >0.35+0.5, Custom: >0.25+0.3, All addon items: >0.15+0.2 (catches every addon)
                    } else if ((std::abs(motion.mvU) > 0.45f || std::abs(motion.mvV) > 0.45f) ||
                               ((std::abs(motion.mvU) > 0.35f || std::abs(motion.mvV) > 0.35f) && cfg.hudProtection > 0.5f) ||
                               ((std::abs(motion.mvU) > 0.25f || std::abs(motion.mvV) > 0.25f) && cfg.hudProtection > 0.3f) ||
                               ((std::abs(motion.mvU) > 0.15f || std::abs(motion.mvV) > 0.15f) && cfg.hudProtection > 0.2f)) {
                        swapResult = mOrigSwapBuffers(dpy, surface);
                        std::swap(mTexPrev, mTexCur);
                    } else {
                    const bool canMultiSwap =
                        (cfg.frameGenMode ==
                         FrameGenMode::MultiSwapInterpolation) &&
                        !prevPacing.autoTuneReduced &&
                        (prevPacing.emaFrameIntervalNs >= 1'000'000LL) &&
                        (prevPacing.emaFrameIntervalNs <= 80'000'000LL);

                    if (canMultiSwap) {
                        const int extraFrames =
                            std::clamp(effectiveMult - 1, 1, 3);
                        const std::int64_t intervalNs =
                            prevPacing.emaFrameIntervalNs;
                        const std::int64_t baseNs = std::max<std::int64_t>(
                            nowNs, mLastPresentedTimestampNs + 1'000'000LL);

                        bool allSwapsOk = true;
                        for (int i = 1; i <= extraFrames; ++i) {
                            const float phase =
                                static_cast<float>(i) /
                                static_cast<float>(extraFrames + 1);
                            const std::int64_t synthTs =
                                baseNs + (intervalNs * i) / (extraFrames + 1);

                            if (mPresentationTimeFn != nullptr) {
                                mPresentationTimeFn(dpy, surface, synthTs);
                            }
                            // LOCK-IN GUARD 4: drawSynthesizedQuad can fail (shader/tiled GPU) — fallback to real if it does
                            if (!drawSynthesizedQuad(motion.mvU, motion.mvV, phase,
                                                cfg.blendStrength,
                                                cfg.hudProtection)) {
                                allSwapsOk = false;
                                ++mConsecutiveGlErrors;
                                break;
                            }
                            if (mOrigSwapBuffers(dpy, surface) != EGL_TRUE) {
                                allSwapsOk = false;
                                break;
                            }
                            mLastPresentedTimestampNs = synthTs;
                            ++synthesizedThisSwap;
                        }

                        if (allSwapsOk) {
                            const std::int64_t finalTs = std::max<std::int64_t>(
                                baseNs + intervalNs,
                                mLastPresentedTimestampNs + 1'000'000LL);
                            if (mPresentationTimeFn != nullptr) {
                                mPresentationTimeFn(dpy, surface, finalTs);
                            }
                            if (!drawSynthesizedQuad(motion.mvU, motion.mvV, 1.0f,
                                                cfg.blendStrength * 0.35f,
                                                cfg.hudProtection)) {
                                ++mConsecutiveGlErrors;
                                swapResult = EGL_FALSE;
                            } else {
                                swapResult = mOrigSwapBuffers(dpy, surface);
                            }
                            if (swapResult == EGL_TRUE) {
                                mLastPresentedTimestampNs = finalTs;
                                std::swap(mTexPrev, mTexCur);
                                activeMultiplier = synthesizedThisSwap + 1;
                                mConsecutiveGlErrors = 0;
                                usedRealWarp = true;
                            } else {
                                ++mConsecutiveGlErrors;
                                resetTemporalState();
                            }
                        } else {
                            ++mConsecutiveGlErrors;
                            resetTemporalState();
                            swapResult = mOrigSwapBuffers(dpy, surface);
                        }
                    } else {
                        // JNGS Single-Swap: one warp at the true midpoint (0.5)
                        // — this is the lowest-latency path. For 3x/4x we
                        // previously did multi-phase averaging which looks like
                        // motion blur and prevents 800 from feeling like 800.
                        // Now we keep it sharp and only vary blend by mult.
                        const float multBoost = cfg.enableMotionBlur
                                ? ((effectiveMult == 4) ? 0.88f : ((effectiveMult == 3) ? 0.80f : 0.72f))
                                : ((effectiveMult == 4) ? 0.42f : ((effectiveMult == 3) ? 0.38f : 0.32f));
                        const float turboBoost = cfg.turboMode ? 1.15f : 1.0f;
                        const float effectiveBlend =
                            prevPacing.autoTuneReduced && !cfg.turboMode
                                ? (cfg.blendStrength * 0.45f)
                                : (cfg.blendStrength * multBoost * turboBoost);
                        const float singlePhase = 0.5f;
                        if (!drawSynthesizedQuad(motion.mvU, motion.mvV, singlePhase,
                                            effectiveBlend, cfg.hudProtection)) {
                            ++mConsecutiveGlErrors;
                            swapResult = mOrigSwapBuffers(dpy, surface);
                            std::swap(mTexPrev, mTexCur);
                        } else {
                            swapResult = mOrigSwapBuffers(dpy, surface);
                        if (swapResult == EGL_TRUE) {
                            if (mConsecutiveGlErrors < 6) {
                                std::swap(mTexPrev, mTexCur);
                                synthesizedThisSwap = effectiveMult - 1;
                                activeMultiplier = effectiveMult;
                                mConsecutiveGlErrors = 0;
                                usedRealWarp = true;
                            } else {
                                std::swap(mTexPrev, mTexCur);
                            }
                        } else {
                            ++mConsecutiveGlErrors;
                            resetTemporalState();
                        }
                        }
                    }
                    } // end guards
                }
            }
        }
        if (synthesizedThisSwap > 0) {
            tryResolveInbuiltFpsCounter();
            if (mInbuiltFpsFrameCount != nullptr) {
                // JNGS: count matches real warps. When we fell back to fake,
                // we already added before the swap; avoid double-add.
                if (usedRealWarp) {
                    mInbuiltFpsFrameCount->fetch_add(synthesizedThisSwap,
                                                     std::memory_order_relaxed);
                }
            }
        }
    }

    const FramePacingMetrics pacing = mFramePacer.recordFrame(
        nowNs, synthesizedThisSwap, activeMultiplier, cfg.autoTune);

    // Also update FpsMod's sCurrentFps (0x178ac) directly so FpsDisplayOverlay's
    // 250ms polling immediately displays the multiplied 2x / 3x / 4x FPS.
    if (synthesizedThisSwap > 0 && mInbuiltCurrentFps != nullptr &&
        pacing.effectiveFps >= 1.0f) {
        const int multipliedFps =
            static_cast<int>(std::lround(pacing.effectiveFps));
        if (multipliedFps > 0) {
            mInbuiltCurrentFps->store(multipliedFps, std::memory_order_relaxed);
        }
    }

    RuntimeTelemetrySnapshot snap{};
    snap.moduleEnabled = modEnabled;
    snap.eglHooked = mHookInstalled.load(std::memory_order_relaxed);
    snap.presentationTimeSupported =
        mPresentationTimeSupported.load(std::memory_order_relaxed);
    snap.turnDeltaHooked = CameraSmoother::instance().isTurnDeltaHooked();
    snap.fovHooked = CameraSmoother::instance().isFovHooked();
    snap.perspectiveHooked = CameraSmoother::instance().isPerspectiveHooked();

    snap.realFps = pacing.realFps;
    snap.effectiveFps = pacing.effectiveFps;
    snap.onePercentLowFps = pacing.onePercentLowFps;
    snap.frameTimeMs = pacing.frameTimeMs;
    snap.jitterMs = pacing.jitterMs;

    snap.lastMotionU = motion.mvU;
    snap.lastMotionV = motion.mvV;
    snap.cameraPitchVelDeg = motion.deltaPitchDeg;
    snap.cameraYawVelDeg = motion.deltaYawDeg;
    snap.currentFovDeg = motion.currentFovDeg;
    snap.cameraPerspective = motion.cameraPerspective;

    snap.totalRealFrames = pacing.totalRealFrames;
    snap.totalSynthesizedFrames = pacing.totalSynthesizedFrames;
    snap.sceneCutResets = mSceneCutCount;
    snap.activeMultiplier = activeMultiplier;
    snap.autoTuneReduced = pacing.autoTuneReduced;

    {
        std::lock_guard<std::mutex> lock(mTelemetryMutex);
        mTelemetry = snap;
    }

    if (postCallback) {
        postCallback(snap);
    }

    return swapResult;
    } catch (...) {
        mConsecutiveGlErrors++;
        if (mOrigSwapBuffers) return mOrigSwapBuffers(dpy, surface);
        return EGL_TRUE;
    }
}

} // namespace framegen
