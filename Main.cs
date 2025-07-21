using DGLabGameController;
using System.Windows.Controls;

namespace GGSThealthMonitor
{
	public class Main : ModuleBase
	{
		public override string Name => "GGST血条监测";

		public override string Info => "v1.0";

		public override string Description => "实时检测当前玩家血条，玩家掉血触发惩罚";

		protected override UserControl CreatePage()
		{
			return new GGSThealthMonitorPage();
		}
	}
}
