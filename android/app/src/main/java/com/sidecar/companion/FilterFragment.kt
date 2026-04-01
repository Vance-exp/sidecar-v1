package com.sidecar.companion

import android.content.pm.ApplicationInfo
import android.os.Bundle
import android.view.LayoutInflater
import android.view.View
import android.view.ViewGroup
import android.widget.TextView
import androidx.fragment.app.Fragment
import androidx.fragment.app.activityViewModels
import androidx.lifecycle.lifecycleScope
import androidx.recyclerview.widget.LinearLayoutManager
import androidx.recyclerview.widget.RecyclerView
import com.sidecar.companion.databinding.FragmentFilterBinding
import com.sidecar.companion.databinding.ItemAppFilterBinding
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.launch
import kotlinx.coroutines.withContext

data class AppInfo(val packageName: String, val label: String)

class FilterFragment : Fragment() {

    private var _binding: FragmentFilterBinding? = null
    private val binding get() = _binding!!

    private val viewModel: SidecarViewModel by activityViewModels()

    /** Packages always suppressed — mirrors SidecarNotifService.BLOCKED_PACKAGES. */
    private val systemBlocked = setOf(
        "android",
        "com.android.systemui",
        "com.android.phone",
        "com.android.launcher3",
        "com.android.launcher",
        "com.google.android.gms",
        "com.google.android.gsf",
        "com.android.settings",
        "com.android.packageinstaller",
        "com.android.providers.downloads",
        "com.android.vending",
        "com.google.android.setupwizard"
    )

    override fun onCreateView(
        inflater: LayoutInflater,
        container: ViewGroup?,
        savedInstanceState: Bundle?
    ): View {
        _binding = FragmentFilterBinding.inflate(inflater, container, false)
        return binding.root
    }

    override fun onViewCreated(view: View, savedInstanceState: Bundle?) {
        super.onViewCreated(view, savedInstanceState)

        val adapter = AppFilterAdapter(emptyList())
        binding.rvApps.layoutManager = LinearLayoutManager(requireContext())
        binding.rvApps.adapter = adapter

        loadApps(adapter)
    }

    override fun onDestroyView() {
        super.onDestroyView()
        _binding = null
    }

    private fun loadApps(adapter: AppFilterAdapter) {
        val ownPkg = requireContext().packageName

        lifecycleScope.launch {
            val apps = withContext(Dispatchers.IO) {
                val pm = requireContext().packageManager
                val intent = android.content.Intent(android.content.Intent.ACTION_MAIN, null).apply {
                    addCategory(android.content.Intent.CATEGORY_LAUNCHER)
                }
                pm.queryIntentActivities(intent, 0)
                    .map { it.activityInfo.packageName }
                    .distinct()
                    .filter { pkg ->
                        pkg != ownPkg && pkg !in systemBlocked
                    }
                    .map { pkg ->
                        val label = try {
                            val info = pm.getApplicationInfo(pkg, 0)
                            pm.getApplicationLabel(info).toString()
                        } catch (_: Exception) {
                            pkg.substringAfterLast('.')
                        }
                        AppInfo(pkg, label)
                    }
                    .sortedBy { it.label.lowercase() }
            }
            adapter.setItems(apps)
        }
    }

    inner class AppFilterAdapter(private var items: List<AppInfo>) :
        RecyclerView.Adapter<AppFilterAdapter.VH>() {

        inner class VH(val binding: ItemAppFilterBinding) : RecyclerView.ViewHolder(binding.root)

        override fun onCreateViewHolder(parent: ViewGroup, viewType: Int): VH {
            val b = ItemAppFilterBinding.inflate(LayoutInflater.from(parent.context), parent, false)
            return VH(b)
        }

        override fun getItemCount() = items.size

        override fun onBindViewHolder(holder: VH, position: Int) {
            val app = items[position]
            val ctx = holder.itemView.context

            holder.binding.tvAppName.text = app.label
            holder.binding.tvAppPkg.text = app.packageName

            // Set switch state without triggering listener
            holder.binding.switchAllowed.setOnCheckedChangeListener(null)
            holder.binding.switchAllowed.isChecked = !AppFilterPrefs.isBlocked(ctx, app.packageName)

            holder.binding.switchAllowed.setOnCheckedChangeListener { _, isChecked ->
                AppFilterPrefs.setBlocked(ctx, app.packageName, !isChecked)
            }
        }

        fun setItems(newItems: List<AppInfo>) {
            items = newItems
            notifyDataSetChanged()
        }
    }
}
