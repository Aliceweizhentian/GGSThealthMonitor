using DGLabGameController;
using System.Windows.Controls;

namespace GGSThealthMonitor
{
	public class Main : ModuleBase
	{
		public override string ModuleId => "GGSTHealthMoniter"; // 模块唯一ID, 与文件夹及DLL名称一致
		public override string Name => "GGST血条监测"; // 模块名称
		public override string Description => "实时检测玩家血条，玩家掉血触发惩罚"; // 模块描述
		public override string Version => "V1.1.1"; // 模块版本
		public override string Author => "Aliceweizhentian"; // 模块作者
		public override int CompatibleApiVersion => 10086; // 兼容的API版本, 通常不会更改

		protected override UserControl CreatePage()
		{
			return new GGSThealthMonitorPage(ModuleId);
		}
	}
}
