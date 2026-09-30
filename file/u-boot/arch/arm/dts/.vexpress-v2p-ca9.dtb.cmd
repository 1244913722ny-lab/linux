cmd_arch/arm/dts/vexpress-v2p-ca9.dtb := mkdir -p arch/arm/dts/ ; (cat arch/arm/dts/vexpress-v2p-ca9.dts; ) > arch/arm/dts/.vexpress-v2p-ca9.dtb.pre.tmp;  cc -E -Wp,-MD,arch/arm/dts/.vexpress-v2p-ca9.dtb.d.pre.tmp -nostdinc -Iinclude   -I./arch/arm/include -include ./include/linux/kconfig.h -I./arch/arm/dts -I./arch/arm/dts/include -I./include -D__ASSEMBLY__ -undef -D__DTS__ -x assembler-with-cpp -o arch/arm/dts/.vexpress-v2p-ca9.dtb.dts.tmp arch/arm/dts/.vexpress-v2p-ca9.dtb.pre.tmp ; ./scripts/dtc/dtc -O dtb -o arch/arm/dts/vexpress-v2p-ca9.dtb -b 0 -i arch/arm/dts/  -Wno-unit_address_vs_reg -Wno-unit_address_format -Wno-avoid_unnecessary_addr_size -Wno-alias_paths -Wno-graph_child_address -Wno-graph_port -Wno-unique_unit_address -Wno-simple_bus_reg -Wno-pci_device_reg -Wno-pci_bridge -Wno-pci_device_bus_num  -@ -a 0x8 -Wno-unit_address_vs_reg -Wno-unit_address_format -Wno-avoid_unnecessary_addr_size -Wno-alias_paths -Wno-graph_child_address -Wno-graph_port -Wno-unique_unit_address -Wno-simple_bus_reg -Wno-pci_device_reg -Wno-pci_bridge -Wno-pci_device_bus_num  -@ -d arch/arm/dts/.vexpress-v2p-ca9.dtb.d.dtc.tmp arch/arm/dts/.vexpress-v2p-ca9.dtb.dts.tmp || (echo "Check /home/ny/QEMU/office_linux/u-boot/arch/arm/dts/.vexpress-v2p-ca9.dtb.pre.tmp for errors" && false) ; sed "s:arch/arm/dts/.vexpress-v2p-ca9.dtb.pre.tmp:arch/arm/dts/vexpress-v2p-ca9.dts:" arch/arm/dts/.vexpress-v2p-ca9.dtb.d.pre.tmp arch/arm/dts/.vexpress-v2p-ca9.dtb.d.dtc.tmp > arch/arm/dts/.vexpress-v2p-ca9.dtb.d

source_arch/arm/dts/vexpress-v2p-ca9.dtb := arch/arm/dts/vexpress-v2p-ca9.dts

deps_arch/arm/dts/vexpress-v2p-ca9.dtb := \
  include/linux/kconfig.h \
    $(wildcard include/config/booger.h) \
    $(wildcard include/config/foo.h) \
    $(wildcard include/config/spl/.h) \
    $(wildcard include/config/tpl/.h) \
    $(wildcard include/config/tools/.h) \
    $(wildcard include/config/tpl/build.h) \
    $(wildcard include/config/vpl/build.h) \
    $(wildcard include/config/spl/build.h) \
    $(wildcard include/config/tools/foo.h) \
    $(wildcard include/config/spl/foo.h) \
    $(wildcard include/config/tpl/foo.h) \
    $(wildcard include/config/vpl/foo.h) \
    $(wildcard include/config/option.h) \
    $(wildcard include/config/acme.h) \
    $(wildcard include/config/spl/acme.h) \
    $(wildcard include/config/tpl/acme.h) \
    $(wildcard include/config/if/enabled/int.h) \
    $(wildcard include/config/int/option.h) \
  arch/arm/dts/vexpress-v2m.dtsi \

arch/arm/dts/vexpress-v2p-ca9.dtb: $(deps_arch/arm/dts/vexpress-v2p-ca9.dtb)

$(deps_arch/arm/dts/vexpress-v2p-ca9.dtb):
