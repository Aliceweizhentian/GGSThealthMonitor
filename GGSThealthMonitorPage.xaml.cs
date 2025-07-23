using DGLabGameController;
using lyqbing.DGLAB;
using System;
using System.Runtime.InteropServices;
using System.Windows;
using System.Windows.Controls;
using System.Windows.Threading;
using System.IO; 
using System.Reflection; 

namespace GGSThealthMonitor
{
	[StructLayout(LayoutKind.Sequential)]
	public struct PlayerPositions
	{
		public int NetPosition;
		public int LocalPosition;
	}
	public static class MemoryMonitor
	{
		private const string DllPath = "MemoryMonitor.dll";

		[UnmanagedFunctionPointer(CallingConvention.Cdecl)]
		public delegate void HealthChangedCallback(int playerId, int newHealth, int oldHealth);

		[DllImport("kernel32.dll", CharSet = CharSet.Unicode, SetLastError = true)]
		[return: MarshalAs(UnmanagedType.Bool)]
		public static extern bool SetDllDirectory(string lpPathName);


		[DllImport(DllPath, CallingConvention = CallingConvention.Cdecl, CharSet = CharSet.Unicode, SetLastError = true)]
		public static extern bool InitializeMonitor(
			string processName,
			string moduleName,
			ulong baseOffset1P,
			ulong[] offsets1P,
			int offsetsCount1P,
			ulong baseOffset2P,
			ulong[] offsets2P,
			int offsetsCount2P,
			ulong netFightPlaceOffset,
			ulong localFightPlaceOffset,
			HealthChangedCallback callback);

		[DllImport(DllPath, CallingConvention = CallingConvention.Cdecl)]
		public static extern void StartMonitoring();

		[DllImport(DllPath, CallingConvention = CallingConvention.Cdecl)]
		public static extern void StopMonitoring();

		[DllImport(DllPath, CallingConvention = CallingConvention.Cdecl)]
		public static extern PlayerPositions GetPlayerPositions();

		[DllImport(DllPath, CallingConvention = CallingConvention.Cdecl)]
		public static extern int GetPlayerHealth(int playerId);
	}
	
	public partial class GGSThealthMonitorPage : UserControl, IDisposable
	{
		/// <summary>
		/// 模块目录
		/// </summary>
		private string moduleFolderPath;
		/// <summary>
		/// 用于在惩罚结束后恢复基础强度的定时器
		/// </summary>
		private readonly DispatcherTimer _punishmentTimer;
		/// <summary>
		/// 每次惩罚持续的秒数
		/// 目前定为2秒
		/// </summary>
		private double punishmentDurationSeconds = 2.0;

		/// <summary>
		/// 惩罚的倍数
		/// </summary>
		private double mutible = 1.0;
		/// <summary>
		/// 惩罚的上限
		/// </summary>
		private int limit = 200;
		/// <summary>
		/// 基础强度
		/// </summary>
		private int baseStrength = 20;
		/// <summary>
		/// 当前惩罚状态下的强度值
		/// </summary>
		private int _currentPunishmentIntensity = 0;

		/// <summary>
		/// 连段累加
		/// 当连读触发时，如果当前受到的伤害小于上次惩罚的伤害
		/// 那么继续沿用上次惩罚的伤害并加上一个comboPunish
		/// </summary>
		private int combopunish = 1;

		/// <summary>
		/// 不分敌我模式开关，谁掉血都要电
		/// </summary>
		private bool punishAllPlayers = false; 

		/// <summary>
		/// 是否为本地对战
		/// </summary>
		private bool isLocalFight = false;

		/// <summary>
		/// 保证委托在C++调用期间不会被垃圾回收。
		/// </summary>
		private static MemoryMonitor.HealthChangedCallback _callbackDelegate;
		private bool isMonitoring = false;

		public GGSThealthMonitorPage(string moduleId)
		{
			moduleFolderPath = Path.Combine(ConfigManager.ModulesPath, moduleId);
			_punishmentTimer = new DispatcherTimer();
			SetupDllDirectory(moduleFolderPath);
			InitializeComponent();
		}


		public void Dispose()
		{
			if (isMonitoring)
			{
				MemoryMonitor.StopMonitoring();
			}
			_punishmentTimer?.Stop();
			GC.SuppressFinalize(this);
		}

		/// <summary>
		/// 计算 MemoryMonitor.dll 的路径并添加到搜索路径中
		/// </summary>
		private void SetupDllDirectory(string targetDllDirectory)
		{
			try
			{
				//废弃
				// // 1. 获取当前正在执行的DLL的完整路径
				// string currentAssemblyPath = Assembly.GetExecutingAssembly().Location;
				// // 2. 获取该DLL所在的目录
				// string modulesDirectory = Path.GetDirectoryName(currentAssemblyPath);
				// // 3. 构造目标DLL所在文件夹的绝对路径 (上一层 -> GGSTmemoryReader)
				// string targetDllDirectory = Path.GetFullPath(Path.Combine(modulesDirectory, @"..\GGSTmemoryReader"));



				//  将这个目录设置为DLL的搜索路径
				bool success = MemoryMonitor.SetDllDirectory(targetDllDirectory);

				if (!success)
				{
					DebugHub.Warning("罪恶装备", "设置DLL搜索路径失败，插件可能无法正常工作。");
				}
			}
			catch (Exception ex)
			{
				DebugHub.Error("罪恶装备", $"在设置DLL路径时发生严重错误: {ex.Message}");
			}
		}

		#region 按钮相关事件
		/// <summary>
		/// 关闭按钮点击事件
		/// </summary>
		private void Back_Click(object sender, RoutedEventArgs e)
		{
			if (Application.Current.MainWindow is MainWindow mw)
			{
				mw.CloseActiveModule();
			}
			else DebugHub.Warning("返回失败", "主人...我不知道该回哪里去呢？");
		}

		/// <summary>
		/// 强度倍数点击事件
		/// </summary>
		private void SetMutible_Click(object sender, RoutedEventArgs e)
		{
			new InputDialog("强度倍数", "输入一个数，角色减少的体力值乘以强度倍数即为受到的电击强度", txtMutible.Text, "设定", "取消",
			(data) =>
			{
				if (!string.IsNullOrWhiteSpace(data.InputText))
				{
					// [优化] 使用 double.TryParse 以支持小数输入
					if (double.TryParse(data.InputText, out double value) && value >= 0 && value <= 100)
					{
						txtMutible.Text = data.InputText;
						mutible = value;
					}
					else DebugHub.Warning("罪恶装备", "请输入0-100之间的数字");
				}
				else DebugHub.Warning("罪恶装备", "请输入一个有效值");
				data.Close();
			},
			(data) => data.Close()).ShowDialog();
		}

		/// <summary>
		/// 强度上限设置点击事件
		/// </summary>
		public void SetLimit_Click(object sender, RoutedEventArgs e)
		{
			new InputDialog("强度上限", "输入一个上限值，受到的电击强度不会超过这个值", txtLimit.Text, "设定", "取消",
			(data) =>
			{
				if (!string.IsNullOrWhiteSpace(data.InputText))
				{
					if (int.TryParse(data.InputText, out int value) && value >= 0 && value <= 200)
					{
						txtLimit.Text = data.InputText;
						limit = value;
					}
					else DebugHub.Warning("罪恶装备", "请输入0-200之间的整数");
				}
				else DebugHub.Warning("罪恶装备", "请输入一个有效的容差值");
				data.Close();
			},
			(data) => data.Close()).ShowDialog();
		}

		/// <summary>
		/// 本地对战开关
		/// </summary>
		private void LocalFightToggle_Click(object sender, RoutedEventArgs e)
		{
			// ToggleButton的IsChecked是bool?（可空类型），我们将其转换为bool
			isLocalFight = LocalFightToggle.IsChecked ?? false;

			if (isLocalFight)
			{
				DebugHub.Log("罪恶装备", "已开启【本地对战】模式。");
			}
			else
			{
				DebugHub.Log("罪恶装备", "已关闭【本地对战】模式。");
			}
		}

		/// <summary>
		/// “不分敌我”模式开关点击事件
		/// </summary>
		private void AllPunishToggle_Click(object sender, RoutedEventArgs e)
		{
			// ToggleButton的IsChecked是bool?（可空类型），我们将其转换为bool
			punishAllPlayers = AllPunishToggle.IsChecked ?? false;

			if (punishAllPlayers)
			{
				DebugHub.Log("罪恶装备", "已开启【不分敌我】模式。");
			}
			else
			{
				DebugHub.Log("罪恶装备", "已关闭【不分敌我】模式。");
			}
		}

		/// <summary>
		/// 基础强度设置点击事件
		/// </summary>
		public void SetBaseStrength_Click(object sender, RoutedEventArgs e)
		{
			new InputDialog("基础强度", "什么也不做时的基础强度", txtBaseStrength.Text, "设定", "取消",
			(data) =>
			{
				if (!string.IsNullOrWhiteSpace(data.InputText))
				{
					if (int.TryParse(data.InputText, out int value) && value >= 0 && value <= 200)
					{
						txtBaseStrength.Text = data.InputText;
						baseStrength = value;
					}
					else DebugHub.Warning("罪恶装备", "请输入0-200之间的整数");
				}
				else DebugHub.Warning("罪恶装备", "请输入一个有效的容差值");
				data.Close();
			},
			(data) => data.Close()).ShowDialog();
		}
		/// <summary>
		/// 连段惩罚设置点击事件
		/// </summary>
		public void SetComboPunish_Click(object sender, RoutedEventArgs e)
		{
			new InputDialog("连段惩罚", "连段时的惩罚值", txtComboPunish.Text, "设定", "取消",
			(data) =>
			{
				if (!string.IsNullOrWhiteSpace(data.InputText))
				{
					if (int.TryParse(data.InputText, out int value) && value >= 0)
					{
						txtComboPunish.Text = data.InputText;
						combopunish = value;
					}
				}
				else DebugHub.Warning("罪恶装备", "请输入一个有效的值");
				data.Close();
			},
			(data) => data.Close()).ShowDialog();
		}

		/// <summary>
		/// 修改惩罚计时器时长点击事件
		/// </summary>
		public void SetpunishTimer_Click(object sender, RoutedEventArgs e)
		{
			new InputDialog("惩罚时长", "受到伤害之后的惩罚持续时间,单位为秒,支持小数", txtPunishmentDuration.Text, "设定", "取消",
			(data) =>
			{
				if (!string.IsNullOrWhiteSpace(data.InputText))
				{
					if (double.TryParse(data.InputText, out double value) && value > 0)
					{
						punishmentDurationSeconds = value;

						txtPunishmentDuration.Text = value.ToString("F1");

						if (isMonitoring)
						{
							_punishmentTimer.Interval = TimeSpan.FromSeconds(punishmentDurationSeconds);
							DebugHub.Log("罪恶装备", $"惩罚时长已实时更新为 {punishmentDurationSeconds} 秒。");
						}
					}

				}
				else DebugHub.Warning("罪恶装备", "请输入一个有效的值");
				data.Close();
			},
			(data) => data.Close()).ShowDialog();
		}

		/// <summary>
		/// 开始监控按钮点击事件
		/// </summary>
		private void BtnStartMonitor_Click(object sender, RoutedEventArgs e)
		{
			if (isMonitoring) return;

			_punishmentTimer.Tick += PunishmentTimer_Tick; // 指定定时器触发时要执行的方法
			_punishmentTimer.Interval = TimeSpan.FromSeconds(punishmentDurationSeconds);

			// 1. 创建委托实例，指向我们的处理函数
			_callbackDelegate = new MemoryMonitor.HealthChangedCallback(OnHealthChanged);

			// 2. 定义游戏进程和地址信息
			const string processName = "GGST-Win64-Shipping.exe";
			const string moduleName = "GGST-Win64-Shipping.exe";


			ulong health_address_baseoffset_1p = 0x051B4158;
			ulong[] health_offsets_1p = { 0x1C0, 0x28, 0x1220 };

			ulong health_address_baseoffset_2p = 0x051B4158;
			ulong[] health_offsets_2p = { 0x1C0, 0x1A0, 0x1220 };

			ulong net_fight_place_address_offset = 0x4D383F4;
			ulong local_fight_place_address_offset = 0x4541FCC;

			// 3. 调用初始化函数，将委托传递给 C++
			bool success = MemoryMonitor.InitializeMonitor(
				processName, moduleName,
				health_address_baseoffset_1p, health_offsets_1p, health_offsets_1p.Length,
				health_address_baseoffset_2p, health_offsets_2p, health_offsets_2p.Length,
				net_fight_place_address_offset, local_fight_place_address_offset,
				_callbackDelegate
			);

			if (!success)
			{
				DebugHub.Warning("罪恶装备", "未能初始化监控器。请确保游戏正在运行，并且DLL文件位于正确的位置。");
				return;
			}

			// 4. 启动监控
			MemoryMonitor.StartMonitoring();
			isMonitoring = true;
			DebugHub.Log("罪恶装备", "正在监听血量变化...");

			_ = DGLab.SetStrength.Set(baseStrength);
			DebugHub.Log("罪恶装备", $"监控已启动，基础强度为 {baseStrength}。");



			btnStart.IsEnabled = false;
			btnStop.IsEnabled = true; // 您的停止按钮名为 btnStart，不是 BtnStopMonitor
		}

		private void BtnStopMonitor_Click(object sender, RoutedEventArgs e)
		{
			if (!isMonitoring) return;

			MemoryMonitor.StopMonitoring();
			isMonitoring = false;
			DebugHub.Log("罪恶装备", "已停止监听。");

			_punishmentTimer.Stop();
			_ = DGLab.SetStrength.Set(baseStrength);

			_currentPunishmentIntensity = 0;
			
			DebugHub.Log("罪恶装备", "已停止监听，强度已恢复至基础值。");


			btnStart.IsEnabled = true;
			btnStop.IsEnabled = false; // 您的停止按钮名为 btnStart
		}

		/// <summary>
		/// 这是我们的回调处理函数，当C++检测到血量变化时会执行此方法。
		/// 注意：这个方法是在一个后台线程中被调用的！
		/// </summary>
		private void OnHealthChanged(int playerId, int newHealth, int oldHealth)
		{
			// 1. 调用新函数获取包含两个位置的结构体
			PlayerPositions positions = MemoryMonitor.GetPlayerPositions();

			int myPlayerPosition = 0;
			if (isLocalFight)
			{
				myPlayerPosition = positions.LocalPosition;
			}
			else
			{
				myPlayerPosition = positions.NetPosition;
			}
			bool isMyPlayer1P = myPlayerPosition % 2 == 1;


			if (punishAllPlayers || ((playerId == 1 && isMyPlayer1P) || (playerId == 2 && !isMyPlayer1P)))
			{
				int damage = oldHealth - newHealth;

				//因为防御下会磨血，因此先简单的把10血以下的认为是在磨血，不处理
				if (damage > 10)
				{
					int shockIntensity = (int)(damage * mutible);
					shockIntensity = Math.Min(shockIntensity, limit);

					Application.Current.Dispatcher.Invoke(() =>
					{
						_punishmentTimer.Stop();

						if (shockIntensity > _currentPunishmentIntensity)
						{
							_ = DGLab.SetStrength.Set(shockIntensity);
							_currentPunishmentIntensity = shockIntensity;

							txtCurrentPercent.Text = $"玩家 {playerId} 受到 {damage} 点伤害，输出 {shockIntensity} 强度";
						}
						else
						{
							// 新的伤害强度不足以覆盖当前惩罚，但我们仍然重置计时器，加上一个惩罚值
							int comboShockIntensity = _currentPunishmentIntensity + combopunish;
							comboShockIntensity = Math.Min(comboShockIntensity, limit); // 确保连段惩罚也不超过上限

							_ = DGLab.SetStrength.Set(comboShockIntensity);
							txtCurrentPercent.Text = $"玩家 {playerId} 受到 {damage} 点伤害由于连段修正\n进行连段惩罚输出{comboShockIntensity} 强度";

							// 更新当前的惩罚强度记录
							_currentPunishmentIntensity = comboShockIntensity;
						}

						_punishmentTimer.Start();


					});
				}
			}
		}
		/// <summary>
		/// 当惩罚计时器到点时，执行此方法
		/// </summary>
		private void PunishmentTimer_Tick(object sender, EventArgs e)
		{
			// 1. 停止计时器，让它成为一个"一次性"的定时器
			_punishmentTimer.Stop();

			// 2. 将强度恢复到基础值
			_ = DGLab.SetStrength.Set(baseStrength);

			// 当惩罚时间结束时，重置当前惩罚强度记录
			_currentPunishmentIntensity = 0;
		}

		#endregion
	}
}