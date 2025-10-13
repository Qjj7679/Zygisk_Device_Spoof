# 配置文件说明

## 配置格式

配置文件为 JSON 格式，包含一个 `apps` 数组，每个元素代表一个需要伪装的应用。

## 字段说明

- `package`: 应用包名（必需，精确匹配）
- `brand`: 设备品牌，对应 `Build.BRAND`
- `model`: 设备型号，对应 `Build.MODEL`
- `manufacturer`: 制造商，对应 `Build.MANUFACTURER`
- `device`: 设备代号，对应 `Build.DEVICE`
- `product`: 产品名称，对应 `Build.PRODUCT`

## 示例配置

```json
{
  "apps": [
    {
      "package": "com.example.targetapp",
      "brand": "Google",
      "model": "Pixel 7",
      "manufacturer": "Google",
      "device": "cheetah",
      "product": "cheetah"
    }
  ]
}
```

## 常见设备信息

### Google Pixel 系列
- **Pixel 7**: brand=Google, model=Pixel 7, device=cheetah, product=cheetah
- **Pixel 7 Pro**: brand=Google, model=Pixel 7 Pro, device=cheetah, product=cheetah
- **Pixel 8**: brand=Google, model=Pixel 8, device=shiba, product=shiba

### Samsung Galaxy 系列
- **Galaxy S23**: brand=Samsung, model=Galaxy S23, device=dm3q, product=dm3qxxx
- **Galaxy S23 Ultra**: brand=Samsung, model=Galaxy S23 Ultra, device=dm3q, product=dm3qxxx
- **Galaxy S24**: brand=Samsung, model=Galaxy S24, device=e3q, product=e3qxxx

### Xiaomi 系列
- **Xiaomi 13**: brand=Xiaomi, model=2211133C, device=fuxi, product=fuxi
- **Xiaomi 14**: brand=Xiaomi, model=24031PN0DC, device=houji, product=houji

## 热重载

配置文件修改后会自动同步到缓存文件，新启动的应用会使用新配置。

## 注意事项

1. 包名必须精确匹配，不支持通配符
2. 所有字段都是可选的，但建议填写完整
3. 配置文件格式错误会导致模块无法加载
4. 修改配置后需要重启目标应用才能生效


